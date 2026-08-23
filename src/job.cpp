// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "cineform/job.hpp"

namespace cineform {

// THE TRAILING-BYTE CHECK THAT USED TO LIVE HERE IS GONE, because it is in the harness now.
//
// `weft::cbor::Reading` claimed its well-formedness walk refused trailing bytes and did not:
// four of four appended bytes were accepted on a 99-byte encoded job, and two concatenated
// messages were read as one with the second dropped. This file carried its own
// `single_top_level_map` against that, which was the right thing to do while the defect
// stood and the wrong thing to keep afterwards -- a second copy of a rule is the drift the
// shared header exists to prevent.
//
// contract-bus 4340ea343941 fixes it at the source, with `proof/cbor_shape.cpp` as the gate
// that was missing: 5 of its 14 checks fail against the old walk and all 14 pass against the
// new one. This repository pins that commit, so `Reading::ok()` below is now load-bearing
// rather than merely first.

bool job_decode(const unsigned char *body, size_t len, Job *out) {
	weft::cbor::Reading r(body, len);
	if (!r.ok()) {
		return false;
	}

	// Each read is asked for and may be declined. A declined field keeps the default the
	// struct was constructed with, which is the whole reason the defaults live in the header:
	// an older transport layer that omits `threads` gets the same 0 an explicit 0 would give.
	std::string s;
	if (r.text("input", s)) {
		out->input = s;
	}
	if (r.text("output", s)) {
		out->output = s;
	}

	uint64_t v = 0;
	if (r.uint("width", v)) {
		out->width = uint32_t(v);
	}
	if (r.uint("height", v)) {
		out->height = uint32_t(v);
	}
	if (r.uint("fps", v)) {
		out->fps = uint32_t(v);
	}
	if (r.uint("quality", v)) {
		out->quality = uint32_t(v);
	}
	if (r.uint("threads", v)) {
		out->threads = uint32_t(v);
	}
	if (r.uint("source", v)) {
		out->source = uint32_t(v);
	}
	if (r.uint("total_frames", v)) {
		out->total_frames = v;
	}
	if (r.uint("mix_rate", v)) {
		out->mix_rate = uint32_t(v);
	}
	if (r.uint("channels", v)) {
		out->channels = uint32_t(v);
	}
	if (r.uint("audio_bits", v)) {
		out->audio_bits = uint32_t(v);
	}

	bool b = false;
	if (r.boolean("keep_alpha", b)) {
		out->keep_alpha = b;
	}
	return true;
}

size_t job_encode(const Job &job, unsigned char *buf, size_t cap) {
	weft::cbor::Map m(buf, cap);
	m.text("input", job.input.c_str());
	m.text("output", job.output.c_str());
	m.uint("width", job.width);
	m.uint("height", job.height);
	m.uint("fps", job.fps);
	m.uint("quality", job.quality);
	m.uint("threads", job.threads);
	m.uint("source", job.source);
	m.uint("total_frames", job.total_frames);
	m.uint("mix_rate", job.mix_rate);
	m.uint("channels", job.channels);
	m.uint("audio_bits", job.audio_bits);
	m.boolean("keep_alpha", job.keep_alpha);
	return m.finish();
}

} // namespace cineform
