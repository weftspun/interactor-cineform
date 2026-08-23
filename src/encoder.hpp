// The CineForm encoder pool, and the buffer lifetime that is the whole difficulty.
//
// CFHD_EncodeAsyncSample does not copy. It queues a job holding the caller's pointer and
// returns immediately, so a staging buffer reused for the next frame is a buffer the pool is
// still reading. The symptom is not a crash: it is a file that decodes to frames blended with
// their successors, which looks like a codec bug and is not one.
//
// So every submitted frame owns a buffer, held in `inflight_` keyed by the frame number the
// pool will hand back, and released only when that number returns. This is the same
// arrangement `godot-cineform` arrived at, and it is recorded here rather than inherited
// silently because the failure it prevents is invisible in every layer above.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT
#ifndef CINEFORM_ENCODER_HPP
#define CINEFORM_ENCODER_HPP

// <cstddef> BEFORE the SDK header, and the order is load-bearing. CFHDAllocator.h, which
// CFHDEncoder.h pulls in, declares `size_t` parameters without including anything that
// defines it. libc++ on Windows happens to have provided it transitively, so this built
// there for weeks; gcc on Linux does not, and reports three "'size_t' has not been
// declared" errors inside a vendored header that has not changed.
#include <cstddef>

#include "CFHDEncoder.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cineform {

class Encoder {
public:
	// Called once per encoded sample, in submission order, with the compressed bytes.
	using OnSample = std::function<void(const void *data, size_t len)>;

	bool start(uint32_t width, uint32_t height, uint32_t quality, uint32_t threads, bool keep_alpha,
			std::string *error);

	// Takes one packed 8-bit RGBA frame, `width*height*4` bytes. Swizzles to BGRA with rows
	// reversed into a buffer this object owns, then submits it.
	bool submit(const uint8_t *rgba, const OnSample &on_sample, std::string *error);

	// Drains completed samples. `block` waits for everything still in flight, which is what
	// the end of a file needs; without it the tail of the movie is lost.
	void drain(bool block, const OnSample &on_sample);

	void stop();

	uint64_t submitted() const { return submitted_; }
	uint32_t queued() const { return uint32_t(inflight_.size()); }

	~Encoder() { stop(); }

private:
	CFHD_EncoderPoolRef pool_ = nullptr;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	uint64_t submitted_ = 0;

	// Keyed by the frame number handed to CFHD_EncodeAsyncSample. std::map rather than a
	// vector because the pool returns numbers, not positions, and an erase must not move the
	// buffers the pool is still holding pointers into.
	std::map<uint32_t, std::vector<uint8_t>> inflight_;
};

} // namespace cineform

#endif
