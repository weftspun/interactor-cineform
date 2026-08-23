// What crosses the bus between a cineform transport layer and this interactor.
//
// THREE SERVICES, NOT TWO, AND THE THIRD IS THE REASON THIS HEADER EXISTS.
// `weft/command.hpp` already carries a command and its reply, correlated by an 8-byte
// request id, on a DYNAMIC byte slice. That pair is request-and-answer: one command, one
// reply, at the end. An ffmpeg-like display is not that shape. It wants a line per frame
// while the work is still running, and a job that answers once at the end has nothing to
// draw for the minutes in between.
//
// So progress rides a third service, and it is FIXED_SIZE rather than DYNAMIC. That is the
// payload variant `contract-bus`'s own `proof/publisher.cpp` has actually run
// (`iox2_type_variant_e_FIXED_SIZE`), where DYNAMIC is the newer one `weft/command.hpp`
// introduces. Progress is fixed-width anyway -- a frame count is a frame count -- so taking
// the proven variant costs nothing and is one less unproven thing under a display.
//
// The subscriber's buffer IS the ring. iceoryx2 keeps the last `buffer_size` samples and
// overwrites the oldest, so a transport layer that redraws at 30 Hz while the encoder emits
// at 900 fps drops stale frames rather than blocking the encoder behind a slow terminal. A
// dropped progress sample costs one skipped line on a status display and nothing else --
// which is exactly why progress is on its own service and not folded into the reply, where
// a drop would lose the result.
//
// NO FLOATS ON THE WIRE. fps and speed are carried as integers scaled by 1000. A float's
// bit pattern is a promise about two compilers agreeing that nothing here checks, and the
// display can divide.
//
// NO NULLS, in the sense the workspace normal form means it. `total_frames == 0` says the
// input length is not known yet, and 0 is a value: it is what an unbounded stdin source
// reports for its whole run, not a hole where a number should be.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT
#ifndef CINEFORM_WIRE_HPP
#define CINEFORM_WIRE_HPP

#include <cstdint>

namespace cineform {

// The command and reply services are the harness's own, unchanged, because a transport
// layer that can already reach an interactor must not need a second dialect to reach this
// one. Only progress is new.
inline constexpr const char *PROGRESS_SERVICE_NAME = "weft/cineform/progress";
inline constexpr const char *PROGRESS_TYPE = "cineform::Progress";

// Samples the progress subscriber holds before the oldest is overwritten.
//
// APPLIED ON BOTH ENDS, and it takes two calls rather than one. The service caps what a
// subscriber may ask for and iceoryx2's default cap is 2, so setting only the subscriber's
// depth does not clamp -- it fails to create the subscriber at all. `progress_bus.hpp` sets
// the service cap and the subscriber depth to this same value.
//
// RETRACTED: THIS USED TO SAY THE DEPTH COULD NOT BE SET. The setter was absent from
// `contract-bus`'s `iceoryx2.sigs`, which is the explicit list its dlsym table is generated
// from, and the conclusion drawn was that applying it would mean extending that table
// unverified. That was the wrong conclusion from a true fact: the symbol exists in
// iceoryx2's C ABI, and the list is hand-maintained rather than complete. Extending it is
// ordinary work, and contract-bus b3acc3be2e00 does it with `proof/buffer_size.cpp` as the
// gate -- 64 samples sent with nobody reading, recovering 1 at depth 1 and 16 at depth 16.
//
// The depth still does not stop loss, and is not meant to. A display redraws at tens of
// hertz while the encoder emits at hundreds to thousands, so samples are dropped at any
// depth, and `ProgressSubscriber::latest` drains to the newest waiting sample for that
// reason. What the depth buys is that a display which stalls briefly resumes on a recent
// frame rather than on whatever survived a two-deep buffer.
inline constexpr uint64_t PROGRESS_BUFFER = 256;

enum State : uint32_t {
	STATE_IDLE = 0,
	STATE_RUNNING = 1,
	STATE_DONE = 2,
	STATE_FAILED = 3,
};

// 64 bytes, and every field is fixed-width because the bus is opened as FIXED_SIZE and
// iceoryx2 refuses to connect two services whose payload size or alignment disagree. A
// static_assert below is the only thing standing between a field added here and a silent
// refusal to connect at run time, so it is not decoration.
struct Progress {
	uint64_t request_id;   // which job this belongs to; 0 is no job
	uint64_t frame;        // frames muxed into the container so far
	uint64_t bytes_out;    // bytes the muxer has written
	uint64_t elapsed_ns;   // the INTERACTOR's clock, never the display's
	uint64_t total_frames; // 0 means the source has not said, which is a value

	uint32_t fps_milli;    // frames per second * 1000
	uint32_t speed_milli;  // encode rate / realtime rate * 1000
	uint32_t width;
	uint32_t height;
	uint32_t quality;      // ladder index, 0 low .. 5 filmscan3
	uint32_t state;        // one of State
};

static_assert(sizeof(Progress) == 64, "Progress is the FIXED_SIZE payload; changing its size changes the service");
static_assert(alignof(Progress) == 8, "alignment is part of the payload type detail iceoryx2 matches on");

} // namespace cineform

#endif
