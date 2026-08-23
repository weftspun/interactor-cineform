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

	// Adds the uncompressed PCM audio track. Must be called after open() and before the
	// first frame, because Matroska writes its track entries once, ahead of the clusters.
	//
	// A_PCM/INT/LIT: signed little-endian integers, which is what Godot's own movie writer
	// emits and what needs no CodecPrivate at all. `bits` is 16 or 32.
	//
	// UNCOMPRESSED IS A DECISION, NOT AN OVERSIGHT. A FLAC track was built here first and
	// removed: libFLAC is licence-split (BSD codec, GPL tools) so it needed careful build
	// switches to stay on the right side, and neither library this workspace was pointed at
	// -- miniflac, dr_flac -- can encode at all, so nothing lighter was available. Against
	// CineForm video at roughly 0.47 Gbit/s, 48 kHz stereo 16-bit PCM is 1.5 Mbit/s, about
	// a third of one percent. The compression was not worth the dependency.
	bool add_audio_track(uint32_t mix_rate, uint32_t channels, uint32_t bits);

	// One frame's worth of PCM, timestamped from the sample index it starts at.
	bool add_audio_frame(const void *data, size_t len, uint64_t first_sample);


	// One encoded CineForm sample. Every CineForm frame is a keyframe, so there is no
	// is-this-a-keyframe question to get wrong.
	bool add_frame(const void *data, size_t len);

	// Finalizes and closes. Safe to call twice; the second call does nothing.
	bool close();

	uint64_t bytes_written() const { return bytes_ + audio_bytes_; }
	uint64_t frames_written() const { return frames_; }
	uint64_t audio_frames_written() const { return audio_frames_; }
	bool has_audio() const { return audio_track_ != 0; }

	~MkvWriter() { close(); }

private:
	mkvmuxer::MkvWriter writer_;
	mkvmuxer::Segment segment_;
	uint64_t video_track_ = 0;
	uint64_t audio_track_ = 0;
	uint32_t mix_rate_ = 0;
	uint64_t audio_frames_ = 0;
	uint64_t audio_bytes_ = 0;
	uint32_t fps_ = 0;
	uint64_t frames_ = 0;
	uint64_t bytes_ = 0;
	bool open_ = false;
};

} // namespace cineform

#endif
