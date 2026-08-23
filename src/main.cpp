// interactor-cineform: a command in, a CineForm Matroska out, progress while it runs.
//
// The command loop is `contract-bus`'s, unchanged, so a transport layer that can already
// reach an interactor reaches this one with no new dialect. What is added is the third
// service: `ask` publishes a Progress sample per frame while it works, and answers once at
// the end. A display therefore has something to draw during the minutes between.
//
// ONE JOB AT A TIME, AND THAT IS DELIBERATE. `run_command_loop` is single threaded: a
// command is answered before the next is received. Two encodes at once on one machine would
// contend for the same cores and finish both later than running them in sequence, and the
// encoder pool is already using every core it was given.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "cineform/job.hpp"
#include "cineform/progress_bus.hpp"
#include "cineform/wire.hpp"

#include "weft/cbor.hpp"
#include "weft/command.hpp"
#include "weft/loop.hpp"

#include "encoder.hpp"
#include "mkv_writer.hpp"
#include "source.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Context {
	cineform::ProgressPublisher progress;
	// Rises once per command. It is the interactor's own counter and not the transport's
	// request id, which the loop keeps to itself; a display correlates on this.
	uint64_t job_serial = 0;
};

uint64_t now_ns() {
	return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
							.count());
}

// A refusal is still a reply. An interactor that answers a bad command with silence makes the
// caller wait out its deadline and then guess why, which is a worse answer than an error.
size_t reply_error(unsigned char *reply, size_t cap, const std::string &message) {
	weft::cbor::Map m(reply, cap);
	m.text("status", "error");
	m.text("error", message.c_str());
	return m.finish();
}

size_t run_job(Context *ctx, const cineform::Job &job, unsigned char *reply, size_t cap) {
	cineform::Progress p{};
	p.request_id = ctx->job_serial;
	p.width = job.width;
	p.height = job.height;
	p.quality = job.quality;
	p.total_frames = job.total_frames;
	p.state = cineform::STATE_RUNNING;

	std::string error;

	cineform::Source source;
	if (!source.open(job.source, job.input, job.width, job.height, job.total_frames, &error)) {
		p.state = cineform::STATE_FAILED;
		ctx->progress.send(p);
		return reply_error(reply, cap, error);
	}

	cineform::Encoder encoder;
	if (!encoder.start(job.width, job.height, job.quality, job.threads, job.keep_alpha, &error)) {
		p.state = cineform::STATE_FAILED;
		ctx->progress.send(p);
		return reply_error(reply, cap, error);
	}

	cineform::MkvWriter mkv;
	if (!mkv.open(job.output, job.width, job.height, job.fps, job.keep_alpha)) {
		p.state = cineform::STATE_FAILED;
		ctx->progress.send(p);
		return reply_error(reply, cap, "could not open " + job.output + " for writing");
	}

	// The clock starts after every resource is open, so a slow disk mount is not reported as
	// slow encoding.
	const uint64_t started = now_ns();
	bool mux_failed = false;

	// Muxing happens inside the drain callback, in the pool's completion order, which is
	// submission order. Nothing reorders frames.
	auto on_sample = [&](const void *data, size_t len) {
		if (!mkv.add_frame(data, len)) {
			mux_failed = true;
			return;
		}
		p.frame = mkv.frames_written();
		p.bytes_out = mkv.bytes_written();
		p.elapsed_ns = now_ns() - started;
		if (p.elapsed_ns > 0) {
			// Integer throughout: fps * 1000 without ever forming a float. The multiply is
			// done before the divide so the ratio keeps its precision.
			p.fps_milli = uint32_t(p.frame * 1000ULL * 1000000000ULL / p.elapsed_ns);
			// Speed against realtime: how many seconds of movie per second of wall clock.
			p.speed_milli = job.fps ? uint32_t(uint64_t(p.fps_milli) / job.fps) : 0;
		}
		ctx->progress.send(p);
	};

	std::vector<uint8_t> frame;
	for (;;) {
		if (!source.next(&frame, &error)) {
			break; // end of stream, or a short read that set `error`
		}
		if (!encoder.submit(frame.data(), on_sample, &error)) {
			break;
		}
		if (mux_failed) {
			error = "the muxer refused a frame";
			break;
		}
	}

	// Blocking, and not optional: everything still in the pool is the tail of the movie.
	// Draining is done even on the error path, because the pool holds pointers into buffers
	// that go away when `encoder` does.
	encoder.drain(true, on_sample);
	encoder.stop();

	const bool finalized = mkv.close();
	const uint64_t elapsed = now_ns() - started;

	if (!error.empty() || mux_failed || !finalized) {
		p.state = cineform::STATE_FAILED;
		p.elapsed_ns = elapsed;
		ctx->progress.send(p);
		if (error.empty()) {
			error = finalized ? "the muxer refused a frame"
							  : "the Matroska segment did not finalize";
		}
		return reply_error(reply, cap, error);
	}

	p.state = cineform::STATE_DONE;
	p.elapsed_ns = elapsed;
	ctx->progress.send(p);

	// The reply reports the interactor's own milliseconds. A duration measured outside the
	// process that did the work has been wrong here before, which is why `transport-bus-cli`
	// prints this number and never one of its own.
	weft::cbor::Map m(reply, cap);
	m.text("status", "ok");
	m.text("output", job.output.c_str());
	m.uint("frames", mkv.frames_written());
	m.uint("bytes", mkv.bytes_written());
	m.uint("elapsed_ms", elapsed / 1000000ULL);
	m.uint("width", job.width);
	m.uint("height", job.height);
	m.uint("quality", job.quality);
	return m.finish();
}

size_t ask(void *raw_ctx, const char *command, size_t len, unsigned char *reply, size_t cap,
		int *stop) {
	Context *ctx = static_cast<Context *>(raw_ctx);

	cineform::Job job;
	if (!cineform::job_decode((const unsigned char *)command, len, &job)) {
		// Malformed and missing are different answers. This one is malformed: the bytes were
		// not a well-formed CBOR map, which is a broken sender rather than an older one.
		return reply_error(reply, cap, "command was not a well-formed CBOR map");
	}

	// A job naming no output would encode into the default file name and report success, so
	// the two obviously-empty fields are refused rather than defaulted.
	if (job.output.empty()) {
		return reply_error(reply, cap, "output is empty");
	}
	if (job.width == 0 || job.height == 0) {
		return reply_error(reply, cap, "width and height must both be non-zero");
	}

	ctx->job_serial++;
	(void)stop; // this interactor runs until its process is stopped
	return run_job(ctx, job, reply, cap);
}

} // namespace

int main() {
	Context ctx;

	// The command loop loads the bus itself, but the progress publisher needs it loaded
	// before it can open a node, and it opens first. Loading twice is harmless: the harness
	// caches the handle.
	if (!weft::load_bus()) {
		return 1;
	}
	if (!ctx.progress.open()) {
		// A failure here is fatal rather than degraded. An encoder that runs with no progress
		// service looks identical, from a display, to one that is not running at all.
		return 1;
	}

	std::fprintf(stderr, "interactor-cineform: listening on %s, progress on %s\n",
			weft::COMMAND_SERVICE_NAME, cineform::PROGRESS_SERVICE_NAME);
	return weft::run_command_loop(&ctx, ask);
}
