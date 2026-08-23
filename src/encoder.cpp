// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "encoder.hpp"

#include <algorithm>
#include <thread>

#include <tmmintrin.h>

namespace cineform {

namespace {

// 0 low .. 5 filmscan3. The ladder is the SDK's own enum in ascending quality order; it is
// written out rather than computed because the values are not contiguous.
const CFHD_EncodingQuality QUALITY_LADDER[] = {
	CFHD_ENCODING_QUALITY_LOW,
	CFHD_ENCODING_QUALITY_MEDIUM,
	CFHD_ENCODING_QUALITY_HIGH,
	CFHD_ENCODING_QUALITY_FILMSCAN1,
	CFHD_ENCODING_QUALITY_FILMSCAN2,
	CFHD_ENCODING_QUALITY_FILMSCAN3,
};
constexpr uint32_t QUALITY_COUNT = sizeof(QUALITY_LADDER) / sizeof(QUALITY_LADDER[0]);

} // namespace

bool Encoder::start(uint32_t width, uint32_t height, uint32_t quality, uint32_t threads,
		bool keep_alpha, std::string *error) {
	// The codec works in 8x8 wavelet blocks and reports an odd size late and unhelpfully, so
	// it is rejected here where the message can name the size that was asked for.
	if ((width & 1u) || (height & 1u)) {
		*error = "CineForm requires even dimensions, got " + std::to_string(width) + "x" +
				std::to_string(height);
		return false;
	}
	if (quality >= QUALITY_COUNT) {
		*error = "quality index " + std::to_string(quality) + " is outside the ladder 0..5";
		return false;
	}

	// A pool of zero encoders accepts every frame and fails each one with
	// CFHD_ERROR_UNEXPECTED, which reads as a codec fault rather than a configuration one.
	if (threads == 0) {
		const unsigned hw = std::thread::hardware_concurrency();
		threads = std::clamp<uint32_t>(hw ? hw : 1u, 1u, 16u);
	}

	// Queue length above thread count, so a thread finishing a frame has the next one ready
	// rather than waiting on the submitter. TestCFHD's own defaults are 8 threads and 16 deep.
	const int queue_depth = int(threads) * 2;

	CFHD_Error err = CFHD_CreateEncoderPool(&pool_, threads, queue_depth, nullptr);
	if (err != CFHD_ERROR_OKAY) {
		*error = "CFHD_CreateEncoderPool failed with code " + std::to_string(int(err));
		return false;
	}

	const CFHD_EncodedFormat encoded =
			keep_alpha ? CFHD_ENCODED_FORMAT_RGBA_4444 : CFHD_ENCODED_FORMAT_RGB_444;
	err = CFHD_PrepareEncoderPool(pool_, uint_least16_t(width), uint_least16_t(height),
			CFHD_PIXEL_FORMAT_BGRA, encoded, CFHD_ENCODING_FLAGS_NONE, QUALITY_LADDER[quality]);
	if (err != CFHD_ERROR_OKAY) {
		*error = "CFHD_PrepareEncoderPool failed with code " + std::to_string(int(err));
		return false;
	}

	err = CFHD_StartEncoderPool(pool_);
	if (err != CFHD_ERROR_OKAY) {
		*error = "CFHD_StartEncoderPool failed with code " + std::to_string(int(err));
		return false;
	}

	width_ = width;
	height_ = height;
	submitted_ = 0;
	return true;
}

bool Encoder::submit(const uint8_t *rgba, const OnSample &on_sample, std::string *error) {
	if (pool_ == nullptr) {
		*error = "encoder pool is not started";
		return false;
	}

	const uint32_t pitch = width_ * 4u;
	const uint32_t number = uint32_t(submitted_);

	// The buffer belongs to inflight_ from here until the pool hands `number` back. Nothing
	// else may write it, and it must not be a member reused across frames.
	std::vector<uint8_t> &staging = inflight_[number];
	staging.resize(size_t(pitch) * height_);

	// RGBA to BGRA with the rows reversed: CFHD_PIXEL_FORMAT_BGRA expects bottom-up input.
	//
	// _mm_shuffle_epi8 is SSSE3, and SSSE3 IS A RAISE. An earlier version of this comment
	// said it sat inside the SSE2 floor the codec already requires, and that is simply
	// wrong: SSE2 is the x86-64 baseline every such CPU has, SSSE3 arrived four years later
	// with Core 2 in 2006. So this file compiles with -mssse3 and the binary declines to run
	// on a pre-2006 x86-64 part.
	//
	// That is a deliberate trade rather than an oversight. The alternative is an SSE2-only
	// shuffle, which needs several instructions where pshufb needs one, to widen support to
	// Opteron and Prescott -- parts that predate the 1080p wavelet encoding this exists to
	// do. It is recorded here because a floor nobody wrote down is a floor discovered by a
	// bug report.
	const __m128i swizzle = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
	const uint32_t wide = pitch & ~15u;
	uint8_t *out = staging.data();
	for (uint32_t y = 0; y < height_; y++) {
		const uint8_t *s = rgba + size_t(pitch) * (height_ - 1u - y);
		uint8_t *d = out + size_t(pitch) * y;
		uint32_t b = 0;
		for (; b < wide; b += 16) {
			_mm_storeu_si128((__m128i *)(d + b),
					_mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(s + b)), swizzle));
		}
		// The tail runs when the row is not a multiple of 16 bytes, which is any width not a
		// multiple of 4. Even widths are required, odd multiples of 2 are not excluded.
		for (; b < pitch; b += 4) {
			d[b + 0] = s[b + 2];
			d[b + 1] = s[b + 1];
			d[b + 2] = s[b + 0];
			d[b + 3] = s[b + 3];
		}
	}

	const CFHD_Error err =
			CFHD_EncodeAsyncSample(pool_, number, out, intptr_t(pitch), nullptr);
	if (err != CFHD_ERROR_OKAY) {
		inflight_.erase(number);
		*error = "CFHD_EncodeAsyncSample failed on frame " + std::to_string(number) +
				" with code " + std::to_string(int(err));
		return false;
	}
	submitted_++;

	// Not blocking. Waiting for this frame's sample would idle the rest of the pool, which is
	// the whole reason for an async encoder.
	drain(false, on_sample);
	return true;
}

void Encoder::drain(bool block, const OnSample &on_sample) {
	while (!inflight_.empty()) {
		uint32_t number = 0;
		CFHD_SampleBufferRef buffer = nullptr;
		const CFHD_Error err = block ? CFHD_WaitForSample(pool_, &number, &buffer)
									 : CFHD_TestForSample(pool_, &number, &buffer);
		if (err != CFHD_ERROR_OKAY || buffer == nullptr) {
			// Non-blocking with nothing ready is the ordinary case, not a fault.
			break;
		}
		void *data = nullptr;
		size_t len = 0;
		if (CFHD_GetEncodedSample(buffer, &data, &len) == CFHD_ERROR_OKAY && data != nullptr) {
			on_sample(data, len);
		}
		CFHD_ReleaseSampleBuffer(pool_, buffer);
		// Only now is the input buffer for this frame no longer being read.
		inflight_.erase(number);
	}
}

void Encoder::stop() {
	if (pool_ != nullptr) {
		CFHD_StopEncoderPool(pool_);
		CFHD_ReleaseEncoderPool(pool_);
		pool_ = nullptr;
	}
	inflight_.clear();
}

} // namespace cineform
