# SPU DSP HLE — progress

Goal: replace DOA5U's `mstream_dsp_*` guest SPU audio plugins with correct host
C++, verified by a recorded-I/O oracle. Builds on patch 0020's stop-code
interception (which shipped only a dry *bypass* of these plugins).

## Done

- **Oracle harness** (`oracle/`) — dependency-free C++17, runs without device or
  emulator. `selftest` verifies every effect against an independent reference;
  `gen`/`replay` exercise the capture round-trip. All green:
  `make -C native/spu-hle/oracle test` → `ALL PASS (0 failures)`.
- **Capture format** (`dsp_capture.hpp`) — versioned binary, shared verbatim by
  the on-device record hook and the harness.
- **Effect kernels** (`dsp_effects.hpp`), each oracle-verified:
  - meter (peak/RMS + decay `0.948987` from the disasm) — **complete**.
  - biquad filter + para_eq (RBJ cookbook, TDF-II) — algorithm verified vs the
    analytic transfer function.
  - I3DL2 reverb (Schroeder/Freeverb, decay-time→feedback) — verified vs measured
    RT60.
- **Interception + record plumbing** — `patches/0021-spurs-dsp-effect-hle.patch`
  (applies cleanly on top of 0001–0020, verified). Generalizes patch 0020's DSP
  stop into one reserved code per effect (`0x3F88`–`0x3F8C`); a registration call
  self-registers the vtable, a processing call runs the host effect in place at
  the audio EA. Gated behind `SPU DSP HLE (experimental)`; `SPU DSP HLE record`
  dumps `.dspcap` captures. Arming data patch: `docs/spurs/audio-stub/patch-hle.yml`.

## Verified

- `native/spu-hle/oracle` self-test + round-trip: PASS (host, no device).
- Full patch series 0001–0021 applies to the pinned rpcs3 submodule (652cf60).

## Not done / blockers (need the device)

1. **Ground-truth capture.** No real DOA5 `.dspcap` exists in-repo. The effect
   *algorithms* are verified analytically, but bit/perceptual matching against
   the real SPU output requires an on-device record run. Synthetic captures only
   exercise the plumbing.
2. **Descriptor param offsets.** filter/para_eq/i3dl2 use documented default
   parameters; the coefficient block's byte offsets inside the descriptor are not
   yet pinned. Record mode captures the descriptor so they can be derived. The
   meter is parameter-free and unaffected.
3. **Audio block format.** Assumed interleaved stereo f32, 0x3000 bytes. Record
   mode dumps the raw block to confirm/adjust.
4. **Could not compile the emulator core here.** The `rpcs3` submodule builds
   only as part of the Android core; patch 0021 is written correct-by-construction
   against patch 0020's proven API (`spu.gpr`, `spu._ptr`, `vm::_ptr`, `be_t`,
   `g_cfg`, `fs::file`) and verified to apply cleanly, but is not yet compiled.
5. **ATRAC** (`SPU-702a721b`) — scaffold (passthrough) only; route to ffmpeg next.

## Next steps

1. On device: enable `SPU DSP HLE record`, play a DOA5 fight, pull
   `<cache>/spu-dsp-capture.dspcap`, run `oracle replay` — confirms meter
   bit-exactness and reveals the real audio format.
2. From the captured descriptors, pin the biquad/I3DL2 param offsets; lock
   filter/EQ/reverb to the captured SPU output via the oracle.
3. Measure the in-fight FPS/contention delta with `SPU DSP HLE` on.
4. ATRAC3+ → ffmpeg.
