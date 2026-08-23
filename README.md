# interactor-cineform

A CineForm encoder that answers commands on the weft bus. A job in, a Matroska file out,
and a progress sample for every frame while it runs.

It is the encoder half of a pair. `transport-cineform-tui` sends the job and draws the
result; `service-cineform` stands both of them up on one host. This repository has one
port and it is the bus.

## Where it came from

`entities-godot-cineform` carries a `MovieWriter` for Godot's Movie Maker, and Godot is
the only thing that can drive it. The codec work is not Godot's — an encoder pool, a
swizzle and a muxer — so it is here without the engine, and the engine keeps its own
copy for recording gameplay.

`godot-cineform` is a third arrangement of the same codec: the module as a GDExtension.
It writes **AVI**, and this writes **Matroska**, which is the difference worth naming.
AVI's RIFF size field is 32 bits, so a file stops at 4 GiB, measured at about 56 seconds
of 4K60 CineForm. That is short enough to reach by accident.

## What crosses the bus

Three services. Two are `contract-bus`'s own, unchanged, so a transport layer that can
already reach an interactor reaches this one with no new dialect.

| service                 | payload  | carries                             |
| ----------------------- | -------- | ----------------------------------- |
| `weft/harness/command`  | DYNAMIC  | the job, CBOR, with an 8-byte request id |
| `weft/harness/reply`    | DYNAMIC  | the verdict, once, at the end       |
| `weft/cineform/progress`| FIXED_SIZE | a 64-byte `Progress`, once per frame |

**Pixels do not cross the bus.** `weft::limits::VALUE_BYTES` is 128 KiB and one
1920×1080 RGBA frame is 8,294,400 bytes — about 64 messages for a single frame. The job
names a source the encoder opens itself. ffmpeg's status line does not show you the
pixels either.

**Progress is on its own service, and it is the FIXED_SIZE variant.** That is the one
`contract-bus`'s `proof/publisher.cpp` has actually run; DYNAMIC is newer. Progress is
fixed-width anyway, so taking the proven variant costs nothing. Losing a progress sample
costs one skipped line on a display, which is why it is not folded into the reply, where
a drop would lose the result.

**One job at a time, and scaling means another interactor rather than another thread.**
`run_command_loop` answers a command before receiving the next. That is not a queue depth
waiting to be raised: the encoder pool already takes every core it was given, so a second
concurrent encode would contend with the first and both would finish later than running
them in sequence. Measured at 1920x1080, one job saturates a 16-thread CPU at about
112 fps.

Where more throughput is wanted, the answer is a second interactor in a second container,
which is what RFD 207d already settles for the see-through pair — two interactors on one
host would both receive every command and race to answer it. So this limit is a property
of the architecture rather than a shortfall in this program, and raising it here would
break the correlation the bus depends on.

`include/cineform/wire.hpp` is the single definition of the four things iceoryx2 compares
at connect time — service name, payload type name, size, alignment. The TUI includes that
file rather than restating it, because two copies would be the drift `weft/command.hpp`
exists to prevent one layer up.

## Audio: Godot's own layout, uncompressed

`MovieWriter::write_frame` receives an `Image` and that frame's block of audio together, so
the raw stream interleaves them the same way:

    [RGBA8  w*h*4 bytes][int32 audio  (rate/fps)*channels]
    [RGBA8  w*h*4 bytes][int32 audio  (rate/fps)*channels]
    ...

One stream, so video and audio cannot disagree about length — two separate files can, and
nothing notices until the muxed result is short at one end. `rate/fps` is integer division
because that is how Godot computes it, so an uneven rate drifts in the same direction the
engine's own recording does rather than a different one.

**Samples are int32 on the wire whatever the depth**, because that is what Godot hands over;
its writer takes the top 16 bits for a 16-bit track and so does this. The wire width and the
sample depth are different numbers, and taking one for the other costs 48 dB.

**The track is `A_PCM/INT/LIT` — uncompressed, and that is a decision.** A FLAC track was
built here first and removed. Neither library this workspace was pointed at can encode:
miniflac is a decoder and `V-Sekai.flac` is a playback module over `dr_flac`, also a decoder.
libFLAC does encode, and it is licence-split — BSD codec, GPL tools — so it needed careful
build switches to stay on the right side of the line.

Against CineForm video the compression was not worth the dependency:

| stream                          | rate           |
| ------------------------------- | -------------- |
| CineForm 1080p60, measured      | 0.47 Gbit/s    |
| PCM 48 kHz stereo 16-bit        | 1.5 Mbit/s     |
| audio as a share of the file    | about 0.3 %    |

## Build

Dependencies come from this repository's own `default.xml`, which makes it a `repo`
manifest root.

    repo init -u https://github.com/weftspun/interactor-cineform -m default.xml
    repo sync
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build build --parallel

`CMAKE_POLICY_VERSION_MINIMUM` is needed because cineform-sdk asks for CMake 3.5.1 and
CMake 4 refuses anything below 3.10 without it.

None of the three is a git submodule. `CLAUDE.md` blocklists those.

## What was measured

Built on Windows 11 with clang 22.1.8, CMake 4.4.2, Ninja 1.13.2, against cineform-sdk
at `e9fc59b4c419`, libwebm at `6184f4484a82` and contract-bus at `f9f1ddcd9341`.

**Round trip, through the real bus, on 1920×1080 footage.** Six frames of a CineForm
`gbrap12le` source, decoded to packed RGBA, encoded here, decoded back with FFmpeg 8.1.2
and compared sample by sample. Run by `service-cineform/proof/roundtrip.py`.

| channel | quality 2, RGB_444 | quality 2, RGBA_4444 |
| ------- | ------------------ | -------------------- |
| R       | max 9, 53.84 dB    | max 9, 53.84 dB      |
| G       | max 5, 57.42 dB    | max 5, 57.42 dB      |
| B       | max 8, 53.84 dB    | max 8, 53.84 dB      |
| A       | **not carried**    | max 9, 67.01 dB      |
| ratio   | 11.25:1            | 9.63:1               |

Alpha costs about 15 percent of the file and comes back better than the colour channels
do. Without `-alpha` the encoder is told `CFHD_ENCODED_FORMAT_RGB_444` and alpha is not
carried at all — the first version of the round-trip gate compared it anyway and reported
3.25 dB, which reads as a codec defect and is a test defect.

**Throughput**, from the interactor's own clock, single job, 16-thread CPU:

    1920x1080  6 frames   67 ms    ~112 fps
     640x360  60 frames   40 ms   ~1810 fps

**Audio, through the real bus.** 45 frames of a Godot-style interleaved stream at 320x240,
48 kHz stereo, 16-bit, decoded back with FFmpeg and compared sample by sample:

    decoded samples   144000 of 144000
    differing         0
    worst error       0

Uncompressed, so the bar is exact rather than a quality figure. `ffprobe` reports the two
tracks as `cfhd` and `pcm_s16le`, 45 audio blocks for 45 video frames.

**PROGRESS_BUFFER is applied now**, on both ends. It takes two calls: the service caps what
a subscriber may ask for and iceoryx2's default cap is 2, so setting only the subscriber's
depth fails to create the subscriber rather than clamping it. contract-bus `b3acc3be2e00`
adds both symbols, measured at 64 samples sent with nobody reading — 1 recovered at depth 1,
16 at depth 16.

**Negative controls.** `proof/wire_agreement.cpp` is 33 checks including four that must
fail: a truncated command, a well-formed non-map, trailing bytes past the map, and — the
opposite verdict — an empty map that must be *accepted* as an older sender. A gate that
only ever passes certifies a defect as readily as a fix.

## What this found in its dependencies

Each is merged into the fork or repository it belongs to, and this repository pins the
merged commit. None was sent upstream.

**cineform-sdk did not configure under clang on Windows.** `string(STRIP
${ADDITIONAL_LIBS} ADDITIONAL_LIBS)` gets one argument when neither UNIX nor APPLE is set
and OpenMP is absent. MSVC ships OpenMP, so `OPENMP_FOUND` is true there and the variable
is assigned — which is why `godot-cineform` never hit it. Quoting is the whole fix.

**`weft::cbor::Reading` did not refuse trailing bytes, though its comment said it did.**
The header stated that `Finish` "reports both a malformed item and trailing bytes past the
end". Measured against `f9f1ddcd9341`, with one byte appended to a 99-byte encoded job:

    trailing 0xFF break stop-code   accepted
    trailing 0x01 integer 1         accepted
    trailing 0xA0 empty map         accepted
    trailing 0x00 integer 0         accepted

Four of four accepted, and two concatenated messages were read as one with the second
dropped. The well-formedness walk calls `QCBORDecode_GetNext` until it stops succeeding,
which *consumes* a trailing item, so `Finish` then sees a cleanly exhausted buffer.

**Fixed at the source**, in contract-bus `4340ea343941`, with `proof/cbor_shape.cpp` as the
gate that was missing: 5 of its 14 checks fail against the old walk and all 14 pass against
the new one. This repository pins that commit and carried its own guard until it landed —
a second copy of a rule is the drift the shared header exists to prevent, so the guard is
gone now rather than kept "just in case".

The fix took three attempts, and the two that failed are worth recording because each
looked complete:

| input          | top-level items | consumed | Finish | stop reason      |
| -------------- | --------------- | -------- | ------ | ---------------- |
| `{"a":1}`      | 1               | 4/4      | ok     | 67 NO_MORE_ITEMS |
| `{"a":1}` `01` | 2               | 5/5      | ok     | 67 NO_MORE_ITEMS |
| `{"a":1}` `FF` | 1               | 5/5      | ok     | 32 malformed     |

Counting items misses the last row and comparing consumed length misses it too. Only *why
the walk stopped* separates that row from the first, and only the *count* separates the
second, so both conditions are needed.

## Platforms

x86_64 only, and that is the codec's property rather than a choice. cineform-sdk includes
`<emmintrin.h>` unconditionally in three files and sixteen use `__m128`, with no NEON path
and no scalar fallback. CMake refuses a non-x86_64 target with that explanation rather than
letting it surface hundreds of files deep.

`src/encoder.cpp` additionally requires **SSSE3** for `_mm_shuffle_epi8`, which is a raise
above the x86-64 SSE2 baseline to Core 2 and later (2006). It is stated because a floor
nobody wrote down is a floor found by a bug report.

## Known gaps

**Two input sources, and no image decoder.** Packed 8-bit RGBA, and a deterministic test
pattern that needs no input file. There is no decode-any-image path: a source that guesses
a format from an extension and guesses wrong produces a file rather than an error.

**The test pattern is silent.** It is a video source, and inventing a tone would put a
signal in a corpus nobody asked for. Audio needs a real input stream.

**8-bit in.** The encoder carries 12 bits internally, so nothing is lost here, but this
path cannot record more than it is handed.

## Licence

`Apache-2.0 OR MIT`, matching the CineForm SDK and the rest of the workspace.
