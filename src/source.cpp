// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "source.hpp"

#include "cineform/job.hpp"

#include <cstring>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace cineform {

bool Source::open(uint32_t kind, const std::string &path, uint32_t width, uint32_t height,
		uint64_t total_frames, uint32_t audio_samples_per_frame, uint32_t channels,
		std::string *error) {
	kind_ = kind;
	width_ = width;
	height_ = height;
	total_ = total_frames;
	produced_ = 0;
	audio_per_frame_ = audio_samples_per_frame;
	channels_ = channels;

	if (kind_ == SOURCE_TEST_PATTERN) {
		// A pattern with no length would encode until the machine filled its disk, so an
		// unbounded request is refused rather than defaulted to something arbitrary.
		if (total_ == 0) {
			*error = "the test pattern source needs total_frames; it has no end of its own";
			return false;
		}
		return true;
	}

	if (kind_ != SOURCE_RAW_RGBA) {
		*error = "unknown source kind " + std::to_string(kind_);
		return false;
	}

	if (path == "-") {
		file_ = stdin;
		owns_file_ = false;
#if defined(_WIN32)
		// stdin is text mode on Windows by default, which turns 0x1A into end of file and
		// mangles 0x0D. Both occur constantly in pixel data.
		std::fflush(stdin);
		_setmode(_fileno(stdin), 0x8000 /* _O_BINARY */);
#endif
		return true;
	}

	file_ = std::fopen(path.c_str(), "rb");
	if (file_ == nullptr) {
		*error = "could not open " + path + " for reading";
		return false;
	}
	owns_file_ = true;
	return true;
}

bool Source::next(std::vector<uint8_t> *out, std::vector<int32_t> *audio, std::string *error) {
	const size_t frame_bytes = size_t(width_) * height_ * 4u;
	const size_t audio_count = size_t(audio_per_frame_) * channels_;
	out->resize(frame_bytes);
	audio->assign(audio_count, 0);

	if (total_ != 0 && produced_ >= total_) {
		return false;
	}

	if (kind_ == SOURCE_TEST_PATTERN) {
		// Deterministic in (x, y, frame) alone: no clock, no random state, no dependence on
		// how many frames were produced before. The same frame index renders the same bytes
		// whether it is the first frame of a run or the thousandth.
		uint8_t *p = out->data();
		const uint32_t t = uint32_t(produced_);
		for (uint32_t y = 0; y < height_; y++) {
			for (uint32_t x = 0; x < width_; x++) {
				p[0] = uint8_t((x * 3u + t * 2u) & 0xFFu);
				p[1] = uint8_t((y * 5u + t * 3u) & 0xFFu);
				p[2] = uint8_t(((x ^ y) + t) & 0xFFu);
				p[3] = 0xFFu;
				p += 4;
			}
		}
		produced_++;
		// The pattern is silent. It is a video test source, and inventing a tone would put
		// a signal in a corpus that nothing asked for.
		return true;
	}

	const size_t got = std::fread(out->data(), 1, frame_bytes, file_);
	if (got == 0) {
		return false; // clean end of stream
	}
	if (got != frame_bytes) {
		// A partial frame is a truncated input, not an end. Encoding it would put a half
		// frame of stale buffer into the file and report it as a success.
		*error = "short read on frame " + std::to_string(produced_) + ": wanted " +
				std::to_string(frame_bytes) + " bytes, got " + std::to_string(got);
		return false;
	}

	// The audio block for THIS frame follows its pixels, which is the order Godot hands
	// them over. Reading it here rather than from a second file is what makes a length
	// disagreement impossible instead of merely unlikely.
	if (audio_count != 0) {
		const size_t audio_bytes = audio_count * sizeof(int32_t);
		const size_t got_audio = std::fread(audio->data(), 1, audio_bytes, file_);
		if (got_audio != audio_bytes) {
			// A frame whose audio is missing or short is a truncated stream. Encoding the
			// video and silently dropping the audio would desynchronise everything after
			// it, which is worse than refusing.
			*error = "short audio block after frame " + std::to_string(produced_) +
					": wanted " + std::to_string(audio_bytes) + " bytes, got " +
					std::to_string(got_audio);
			return false;
		}
	}

	produced_++;
	return true;
}

void Source::close() {
	if (file_ != nullptr && owns_file_) {
		std::fclose(file_);
	}
	file_ = nullptr;
	owns_file_ = false;
}

} // namespace cineform
