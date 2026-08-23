// CineForm in Matroska.
//
// Matroska is the workspace container of record, and it is also the one that does not cap
// the file. `godot-cineform` writes AVI, which the SDK's own Example reads, and AVI's 32-bit
// RIFF size field stops at 4 GiB -- measured at about 56 seconds of 4K60 CineForm, which is
// short enough to hit by accident on a real capture.
//
// libwebm supplies the muxer. It writes WebM by default and Matroska when a track carries a
// codec id WebM does not define, which is what `V_MS/VFW/FOURCC` does here.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT
#ifndef CINEFORM_MKV_WRITER_HPP
#define CINEFORM_MKV_WRITER_HPP

#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"

#include <cstdint>
#include <string>

namespace cineform {

class MkvWriter {
public:
	bool open(const std::string &path, uint32_t width, uint32_t height, uint32_t fps, bool keep_alpha);

	// One encoded CineForm sample. Every CineForm frame is a keyframe, so there is no
	// is-this-a-keyframe question to get wrong.
	bool add_frame(const void *data, size_t len);

	// Finalizes and closes. Safe to call twice; the second call does nothing.
	bool close();

	uint64_t bytes_written() const { return bytes_; }
	uint64_t frames_written() const { return frames_; }

	~MkvWriter() { close(); }

private:
	mkvmuxer::MkvWriter writer_;
	mkvmuxer::Segment segment_;
	uint64_t video_track_ = 0;
	uint32_t fps_ = 0;
	uint64_t frames_ = 0;
	uint64_t bytes_ = 0;
	bool open_ = false;
};

} // namespace cineform

#endif
