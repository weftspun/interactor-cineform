// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "cineform/job.hpp"

#include <qcbor/qcbor_decode.h>

namespace cineform {

namespace {

// Exactly one top-level item, and it is a map.
//
// MEASURED, AND IT IS WHY THIS FUNCTION EXISTS. `weft::cbor::Reading` says of its own
// well-formedness walk that "`Finish` is what reports both a malformed item and trailing
// bytes past the end". The first half is true and the second is not. The walk calls
// QCBORDecode_GetNext until it stops succeeding, which CONSUMES any trailing item rather
// than tripping over it, so Finish then sees a cleanly exhausted buffer and reports success.
//
// Against contract-bus at f9f1ddcd9341, a 99-byte encoded job with one byte appended:
//
//     trailing 0xFF break stop-code   accepted
//     trailing 0x01 integer 1         accepted
//     trailing 0xA0 empty map         accepted
//     trailing 0x00 integer 0         accepted
//
// Four of four accepted, where the header's comment promises a refusal. That is a defect in
// the harness rather than in this repository, and it is not worked around silently here: a
// caller appending bytes to a command is a broken sender, and reading the map and discarding
// the rest answers it as though it were a good one.
//
// The count is of items at NESTING LEVEL 0. A map's own item is level 0 and its members are
// level 1, so one top-level item is exactly one map with whatever is inside it, and a second
// level-0 item is trailing data by definition.
bool single_top_level_map(const unsigned char *body, size_t len) {
	QCBORDecodeContext c;
	QCBORDecode_Init(&c, UsefulBufC{body, len}, QCBOR_DECODE_MODE_NORMAL);

	QCBORItem item;
	int top_level = 0;
	bool first_is_map = false;
	QCBORError stopped_because = QCBOR_SUCCESS;
	for (;;) {
		stopped_because = QCBORDecode_GetNext(&c, &item);
		if (stopped_because != QCBOR_SUCCESS) {
			break;
		}
		if (item.uNestingLevel == 0) {
			if (top_level == 0) {
				first_is_map = item.uDataType == QCBOR_TYPE_MAP;
			}
			top_level++;
		}
	}

	// WHY THE WALK STOPPED IS THE VERDICT, and neither the item count nor the consumed length
	// is. Both of those were tried first and both have a hole:
	//
	//     input             top-level items   consumed   Finish   stopped because
	//     {"a":1}                         1        4/4       ok    67 NO_MORE_ITEMS
	//     {"a":1} + 0x01                  2        5/5       ok    67 NO_MORE_ITEMS
	//     {"a":1} + 0xFF                  1        5/5       ok    32 malformed
	//
	// A trailing 0xFF is a break stop-code with no indefinite-length item to close. QCBOR
	// counts it as consumed and Finish reports success over it, so a length comparison sees
	// nothing wrong, and the count stays at one because GetNext refused the byte rather than
	// returning it. Only the stop reason separates the last row from the first.
	//
	// So the rule is: the walk must have run out of items, rather than stopped on one it
	// could not read.
	if (stopped_because != QCBOR_ERR_NO_MORE_ITEMS) {
		return false;
	}
	if (QCBORDecode_Finish(&c) != QCBOR_SUCCESS) {
		return false;
	}
	return top_level == 1 && first_is_map;
}

} // namespace

bool job_decode(const unsigned char *body, size_t len, Job *out) {
	if (!single_top_level_map(body, len)) {
		return false;
	}

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
	m.boolean("keep_alpha", job.keep_alpha);
	return m.finish();
}

} // namespace cineform
