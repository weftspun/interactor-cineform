// An encode job, as it crosses the command service.
//
// The command body is CBOR, because that is what every other interactor on this bus already
// answers: `transport-bus-cli`'s decoder handles maps, text, byte strings and integers, and a
// reply needing more than those is a reply whose shape nobody agreed on. The same four kinds
// are enough to say what to encode.
//
// WHY PIXELS ARE NOT IN HERE. `weft::command::MESSAGE_BYTES` is 128 KiB, request id included.
// One 3840x2160 RGBA frame is 33,177,600 bytes -- about 260 messages for a single frame, and
// 15,552 messages for a second of 60p. Frames do not cross this bus. The job names a source
// the interactor opens itself, and the bus carries the instruction and the progress. That is
// also what ffmpeg's own status line reports on: it does not show you the pixels either.
//
// Every field has a value when the map is short, and the default is written down here rather
// than in the caller, so two transport layers cannot disagree about what "unset" meant.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT
#ifndef CINEFORM_JOB_HPP
#define CINEFORM_JOB_HPP

#include "weft/cbor.hpp"

#include <cstdint>
#include <string>

namespace cineform {

// How the interactor gets frames. A job names one of these; there is no "guess from the
// extension" path, because a guess that is wrong here produces a file rather than an error.
enum SourceKind : uint32_t {
	// Packed 8-bit RGBA, width*height*4 bytes per frame, read until short. "-" is stdin.
	//
	// GODOT'S OWN LAYOUT WHEN mix_rate IS NON-ZERO. `MovieWriter::write_frame` receives an
	// Image and a block of that frame's audio together, so the stream interleaves them the
	// same way: the pixels of frame N, then frame N's audio block, then frame N+1. One
	// stream, and video and audio cannot disagree about length because they are adjacent
	// by construction -- two separate files can, and nothing notices until the muxed
	// result is short at one end.
	SOURCE_RAW_RGBA = 0,
	// The codec's own QBIST pattern generator, the one Example/TestCFHD.cpp encodes. It needs
	// no input file, which is what makes an end-to-end run possible on a bare machine.
	SOURCE_TEST_PATTERN = 1,
};

struct Job {
	std::string input = "-";
	std::string output = "out.mkv";
	uint32_t width = 1920;
	uint32_t height = 1080;
	uint32_t fps = 30;
	// Ladder index into QUALITY_LADDER, 0 low .. 5 filmscan3. 2 is HIGH and is the Godot
	// module's default, kept so a file from either path is the same file.
	uint32_t quality = 2;
	// 0 asks the encoder pool to choose from the processor count. A pool of zero encoders
	// accepts every frame and fails each one with CFHD_ERROR_UNEXPECTED, so 0 must never
	// reach CFHD_CreateEncoderPool as a literal.
	uint32_t threads = 0;
	uint32_t source = SOURCE_RAW_RGBA;
	// 0 means the source has not said how long it is. Carried so a bounded source can drive
	// a percentage and an unbounded one is honest about not having one.
	uint64_t total_frames = 0;
	bool keep_alpha = false;

	// AUDIO. `mix_rate == 0` means the stream carries none, and 0 is a value here rather
	// than a hole: it is what a silent render reports for its whole run, and it is what
	// decides whether the raw stream has audio blocks between its frames at all.
	uint32_t mix_rate = 0;
	uint32_t channels = 2;
	// The PCM sample depth written into the file, 16 or 32. NOT the width of a sample on
	// the wire, which is always int32 because that is what Godot hands over; the two are
	// different numbers, and taking the wire width for the depth costs 48 dB.
	uint32_t audio_bits = 16;
};

// Reads a job out of a command body. Returns false if the message was not a well-formed map;
// a map missing a field is an OLDER sender and keeps the defaults above, which is a different
// answer from a truncated one and is why `weft::cbor::Reading::ok` is checked separately.
bool job_decode(const unsigned char *body, size_t len, Job *out);

// Writes a job into `buf`. Returns the encoded length, or 0 if it did not fit.
size_t job_encode(const Job &job, unsigned char *buf, size_t cap);

} // namespace cineform

#endif
