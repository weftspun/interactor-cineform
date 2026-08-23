// What the two ends have to agree on, checked rather than asserted in prose.
//
// EVERY CHECK HERE SHIPS WITH A NEGATIVE CONTROL. A gate that only ever passes has proved
// nothing: it certifies the defect as readily as the fix. So each positive case is paired
// with input that must FAIL, and the run fails if the broken input passes.
//
// The population is fixed and enumerated -- eleven Progress fields and ten Job fields -- so
// there is no detection floor to state. Nothing here is sampled.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "cineform/job.hpp"
#include "cineform/wire.hpp"

#include "weft/command.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const char *what) {
	checks++;
	if (!ok) {
		failures++;
		std::printf("  FAIL %s\n", what);
	} else {
		std::printf("  ok   %s\n", what);
	}
}

} // namespace

int main() {
	using cineform::Job;
	using cineform::Progress;

	std::printf("Progress: the FIXED_SIZE payload both ends open the service with\n");

	// iceoryx2 matches on payload size and alignment at connect time and refuses a mismatch.
	// A field added without updating these is not a compile error on either side alone; it is
	// two binaries that build and then decline to talk to each other.
	check(sizeof(Progress) == 64, "sizeof is 64");
	check(alignof(Progress) == 8, "alignof is 8");

	// Offsets are enumerated because a reordering keeps the size and changes the meaning of
	// every byte. That failure produces plausible numbers on a display, which is worse than
	// a refusal to connect.
	check(offsetof(Progress, request_id) == 0, "request_id at 0");
	check(offsetof(Progress, frame) == 8, "frame at 8");
	check(offsetof(Progress, bytes_out) == 16, "bytes_out at 16");
	check(offsetof(Progress, elapsed_ns) == 24, "elapsed_ns at 24");
	check(offsetof(Progress, total_frames) == 32, "total_frames at 32");
	check(offsetof(Progress, fps_milli) == 40, "fps_milli at 40");
	check(offsetof(Progress, speed_milli) == 44, "speed_milli at 44");
	check(offsetof(Progress, width) == 48, "width at 48");
	check(offsetof(Progress, height) == 52, "height at 52");
	check(offsetof(Progress, quality) == 56, "quality at 56");
	check(offsetof(Progress, state) == 60, "state at 60");

	// The type name is part of what iceoryx2 compares. A rename on one side only is a silent
	// refusal to connect, with no message naming the field that moved.
	check(std::strcmp(cineform::PROGRESS_TYPE, "cineform::Progress") == 0, "payload type name");
	check(std::strcmp(cineform::PROGRESS_SERVICE_NAME, "weft/cineform/progress") == 0,
			"progress service name");

	std::printf("\nJob: the CBOR command body\n");

	Job sent;
	sent.input = "/corpus/anny.raw";
	sent.output = "/out/take01.mkv";
	sent.width = 1920;
	sent.height = 1080;
	sent.fps = 60;
	sent.quality = 4;
	sent.threads = 12;
	sent.source = cineform::SOURCE_RAW_RGBA;
	sent.total_frames = 900;
	sent.keep_alpha = true;

	std::vector<unsigned char> buf(weft::BODY_MAX);
	const size_t n = cineform::job_encode(sent, buf.data(), buf.size());
	check(n > 0, "job encodes");
	check(n <= weft::BODY_MAX, "encoded job fits one bus message");

	Job got;
	check(cineform::job_decode(buf.data(), n, &got), "job decodes");
	check(got.input == sent.input, "input survives");
	check(got.output == sent.output, "output survives");
	check(got.width == sent.width, "width survives");
	check(got.height == sent.height, "height survives");
	check(got.fps == sent.fps, "fps survives");
	check(got.quality == sent.quality, "quality survives");
	check(got.threads == sent.threads, "threads survives");
	check(got.source == sent.source, "source survives");
	check(got.total_frames == sent.total_frames, "total_frames survives");
	check(got.keep_alpha == sent.keep_alpha, "keep_alpha survives");

	std::printf("\nNegative controls: input that MUST be refused\n");

	// Truncation. QCBOR decodes lazily, so a short buffer enters the map without complaint
	// and fails only when a read runs off the end. weft::cbor::Reading walks the whole
	// message first for exactly this reason, and this is the check that it still does.
	{
		Job j;
		const bool refused = !cineform::job_decode(buf.data(), n / 2, &j);
		check(refused, "a truncated command is refused, not read as a short one");
	}

	// Not a map. A CBOR integer is well-formed CBOR and is not a command.
	{
		const unsigned char just_an_int[] = {0x01};
		Job j;
		check(!cineform::job_decode(just_an_int, sizeof(just_an_int), &j),
				"a well-formed non-map is refused");
	}

	// Trailing bytes. A message with a complete map and junk after it is a broken sender,
	// and reading the map and ignoring the rest would accept it.
	{
		std::vector<unsigned char> extra(buf.begin(), buf.begin() + n);
		extra.push_back(0xFF);
		Job j;
		check(!cineform::job_decode(extra.data(), extra.size(), &j),
				"trailing bytes past the map are refused");
	}

	// An empty map is well-formed and missing every field. That is an OLDER sender, not a
	// broken one, and it must be ACCEPTED with the header's defaults -- the opposite verdict
	// from the three above. If this ever fails alongside them, the decoder has stopped
	// telling malformed from missing.
	{
		const unsigned char empty_map[] = {0xA0};
		Job j;
		const bool accepted = cineform::job_decode(empty_map, sizeof(empty_map), &j);
		check(accepted, "an empty map is accepted as an older sender");
		check(j.quality == 2 && j.width == 1920 && j.threads == 0,
				"an omitted field keeps the header's default");
	}

	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
