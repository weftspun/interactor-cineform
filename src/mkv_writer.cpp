// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "mkv_writer.hpp"

#include <cstdio>
#include <cstring>

namespace cineform {

namespace {

void put_u32le(uint8_t *p, uint32_t v) {
	p[0] = uint8_t(v);
	p[1] = uint8_t(v >> 8);
	p[2] = uint8_t(v >> 16);
	p[3] = uint8_t(v >> 24);
}

void put_u16le(uint8_t *p, uint16_t v) {
	p[0] = uint8_t(v);
	p[1] = uint8_t(v >> 8);
}

} // namespace

bool MkvWriter::open(const std::string &path, uint32_t width, uint32_t height, uint32_t fps,
		bool keep_alpha) {
	if (!writer_.Open(path.c_str())) {
		std::fprintf(stderr, "cineform: could not open %s for writing\n", path.c_str());
		return false;
	}
	if (!segment_.Init(&writer_)) {
		std::fprintf(stderr, "cineform: could not initialize the Matroska segment\n");
		return false;
	}
	segment_.set_mode(mkvmuxer::Segment::kFile);

	video_track_ = segment_.AddVideoTrack(int32_t(width), int32_t(height), 1);
	if (video_track_ == 0) {
		std::fprintf(stderr, "cineform: could not add a video track\n");
		return false;
	}
	mkvmuxer::VideoTrack *vt = (mkvmuxer::VideoTrack *)segment_.GetTrackByNumber(video_track_);
	if (vt == nullptr) {
		return false;
	}

	// AddVideoTrack defaults to VP8. Setting a codec id WebM does not define is what makes
	// libwebm write a DocType of `matroska` rather than `webm`; a player handed a webm
	// DocType with CFHD inside it refuses the file rather than reporting an unknown codec.
	vt->set_codec_id("V_MS/VFW/FOURCC");
	vt->set_frame_rate(double(fps));

	// CodecPrivate for V_MS/VFW/FOURCC is a 40-byte BITMAPINFOHEADER. The fields a decoder
	// actually reads are biCompression and biBitCount; the rest are filled because a short
	// or zeroed header is the kind of thing one demuxer tolerates and the next rejects.
	uint8_t bih[40];
	std::memset(bih, 0, sizeof(bih));
	put_u32le(bih + 0, 40); // biSize
	put_u32le(bih + 4, width);
	put_u32le(bih + 8, height);
	put_u16le(bih + 12, 1); // biPlanes
	put_u16le(bih + 14, keep_alpha ? 32 : 24); // biBitCount
	std::memcpy(bih + 16, "CFHD", 4); // biCompression
	put_u32le(bih + 20, width * height * (keep_alpha ? 4u : 3u)); // biSizeImage
	if (!vt->SetCodecPrivate(bih, sizeof(bih))) {
		std::fprintf(stderr, "cineform: could not set CodecPrivate\n");
		return false;
	}

	fps_ = fps;
	open_ = true;
	return true;
}

bool MkvWriter::add_audio_track(uint32_t mix_rate, uint32_t channels, uint32_t bits) {
	if (!open_) {
		return false;
	}
	audio_track_ = segment_.AddAudioTrack(int32_t(mix_rate), int32_t(channels), 0);
	if (audio_track_ == 0) {
		std::fprintf(stderr, "cineform: could not add an audio track\n");
		return false;
	}
	mkvmuxer::AudioTrack *at = (mkvmuxer::AudioTrack *)segment_.GetTrackByNumber(audio_track_);
	if (at == nullptr) {
		return false;
	}
	at->set_codec_id("A_PCM/INT/LIT");
	at->set_bit_depth(bits);
	mix_rate_ = mix_rate;
	return true;
}

bool MkvWriter::add_audio_frame(const void *data, size_t len, uint64_t first_sample) {
	if (!open_ || audio_track_ == 0) {
		return false;
	}
	// From the sample index rather than accumulated, for the same reason the video track
	// derives its timestamp from the frame index: a per-block rounding error would drift
	// audio away from video over a long file, and drift is what nobody notices until the
	// end.
	const uint64_t timestamp_ns =
			first_sample * 1000000000ULL / (mix_rate_ ? mix_rate_ : 1);
	// Every PCM block stands alone, so each is a keyframe.
	if (!segment_.AddFrame((const uint8_t *)data, len, audio_track_, timestamp_ns, true)) {
		std::fprintf(stderr, "cineform: could not add audio frame %llu to the segment\n",
				(unsigned long long)audio_frames_);
		return false;
	}
	audio_frames_++;
	audio_bytes_ += len;
	return true;
}

bool MkvWriter::add_frame(const void *data, size_t len) {
	if (!open_) {
		return false;
	}
	// Matroska timestamps are absolute nanoseconds, derived from the frame index rather than
	// accumulated, so rounding cannot drift over a long file.
	const uint64_t timestamp_ns = frames_ * 1000000000ULL / (fps_ ? fps_ : 1);
	if (!segment_.AddFrame((const uint8_t *)data, len, video_track_, timestamp_ns, true)) {
		std::fprintf(stderr, "cineform: could not add frame %llu to the segment\n",
				(unsigned long long)frames_);
		return false;
	}
	frames_++;
	bytes_ += len;
	return true;
}

bool MkvWriter::close() {
	if (!open_) {
		return true;
	}
	open_ = false;

	// Matroska takes its duration from the last timestamp, so a file whose final frame has
	// none reports a duration one frame short. set_duration is in milliseconds.
	const uint64_t duration_ns = frames_ * 1000000000ULL / (fps_ ? fps_ : 1);
	segment_.set_duration(double(duration_ns) / 1000000.0);

	const bool finalized = segment_.Finalize();
	if (!finalized) {
		std::fprintf(stderr, "cineform: could not finalize the segment; the file may be unreadable\n");
	}
	writer_.Close();
	return finalized;
}

} // namespace cineform
