// Where frames come in.
//
// Two sources, and the second exists so the pipeline can be run at all on a machine with no
// corpus on it. A test pattern is rendered deterministically from a seed -- same seed, same
// bytes -- which puts it in the CONSTRUCTED half of the synthetic split rather than the
// generated one: nothing was sampled from a learned distribution, so it carries none of the
// four conditions that apply to generated data.
//
// There is no decode-any-image path. A frame source that guesses a format from an extension
// and guesses wrong produces a file rather than an error, and the interactor would then be
// reporting fps for a job that encoded noise.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT
#ifndef CINEFORM_SOURCE_HPP
#define CINEFORM_SOURCE_HPP

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace cineform {

class Source {
public:
	bool open(uint32_t kind, const std::string &path, uint32_t width, uint32_t height,
			uint64_t total_frames, std::string *error);

	// Fills `out` with one packed RGBA frame. Returns false at end of stream, which is not an
	// error; `error` is set only when a read failed or returned a partial frame.
	bool next(std::vector<uint8_t> *out, std::string *error);

	void close();

	~Source() { close(); }

private:
	uint32_t kind_ = 0;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	uint64_t total_ = 0;
	uint64_t produced_ = 0;
	std::FILE *file_ = nullptr;
	bool owns_file_ = false;
};

} // namespace cineform

#endif
