# native/spu-hle — SPU DSP effect HLE + recorded-I/O oracle

Host C++ replacements for DOA5U's `mstream_dsp_*` SPU audio plugins, plus the
oracle that proves them correct. This is step 5 of the pipeline in
`docs/SPU-HLE-PIPELINE.md` ("generate the C++ replacement" + "verify with the
recorded-I/O oracle"), turning patch 0020's DSP *bypass* into real per-effect
host code.

## Files

| file | role |
|---|---|
| `dsp_capture.hpp` | binary capture format shared by the on-device record hook and the host harness. No RPCS3/Android deps. |
| `dsp_effects.hpp` | the effect kernels: meter, biquad filter/EQ, I3DL2 reverb. Pure functions over interleaved PCM. **The single source of truth** — the emulator (patch 0021) includes this exact header, so the game runs what the oracle checks. |
| `oracle/oracle_main.cpp` | host harness: `selftest`, `gen`, `replay`. |
| `oracle/{Makefile,CMakeLists.txt,run.sh}` | dependency-free build. |

## Build & run (no device, no emulator)

```sh
cd native/spu-hle/oracle
make test        # analytic self-test of every effect
make roundtrip   # generate a synthetic capture and replay it
./run.sh         # both, from scratch
```

Only a C++17 compiler is required.

## The effects and how each is verified

The oracle verifies against an **independent** reference, not against the effect
itself, so a passing test is real evidence:

- **meter** (`SPU-b5522754`) — per-channel peak + RMS with an exponential
  peak-hold decay. Verified: peak equals a brute-force `max|x|`, RMS equals a
  brute-force `sqrt(mean(x²))`, and the hold decays by exactly the coefficient
  `0.948987` read out of the plugin's disassembly
  (`docs/spurs/mstream_dsp_meter-disasm.txt`). **Complete** — the meter needs no
  descriptor parameters.
- **filter** (`SPU-2379c584`) / **para_eq** (`SPU-e433af6d`) — RBJ-cookbook
  biquads (low/high-pass, peaking), transposed direct form II. Verified: the
  time-domain impulse response's DFT magnitude matches the closed-form transfer
  function `|H(e^jw)|` at a sweep of frequencies; peaking gain at `f0` equals the
  requested dB; a 3-stage cascade matches the product of the stage responses.
- **i3dl2** (`SPU-6783990e`) — Schroeder/Freeverb comb+allpass network whose
  per-comb feedback is derived from the I3DL2 decay time. Verified: the measured
  RT60 (Schroeder backward integration, T20 fit) tracks the requested decay
  time, the tail is finite/stable, the stereo channels decorrelate, and a shorter
  decay time yields a shorter tail.

## The recorded-I/O oracle

`replay <file.dspcap>` is the mode a **real device capture** goes through: it
reads recorded `(params, input block)`, runs the host effect, and diffs the
result against the recorded output block (bit-exact for the meter/int paths, a
tolerance/SNR gate for float/reverb). An effect ships against DOA5 only when a
real capture passes here.

Capture files are produced on-device by patch 0021's record mode
(`SPU DSP HLE record` config flag → `<cache>/spu-dsp-capture.dspcap`).

## Status / honest limitations

- The **algorithms** are implemented and pass the analytic oracle today; the
  record/replay plumbing round-trips. This runs in CI with no device.
- **No real DOA5 capture ships in this repo yet.** `gen` writes a *synthetic*
  capture (its reference is the effect's own output) to exercise the replay path;
  that is a plumbing check, not ground truth. Ground-truth verification against
  the real SPU output is pending an on-device record run.
- filter/para_eq/i3dl2 currently use **documented default parameters** in the
  emulator bridge — the exact descriptor param-block offsets are not yet pinned.
  Record mode captures the descriptor so they can be derived offline. The meter
  is parameter-free and therefore complete end-to-end (pending the audio-format
  confirmation the same capture provides).
- ATRAC (`SPU-702a721b`) is **scaffold only** (passthrough); it must route to
  ffmpeg (borrow the cellAdec ATRAC HLE) — a separate later target.
