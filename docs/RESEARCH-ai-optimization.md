# Research notes: AI-assisted optimization, SPU HLE, and profiling (2026-08-11)

Session notes from an investigation into whether AI-driven techniques can make
the emulator less CPU-intensive on the Snapdragon targets. Four questions were
worked through, grounded in `docs/PROFILE-doa5.md` and `docs/BENCHMARKS.md` and
in the rpcs3 submodule source (pinned `652cf60`). One deliverable shipped:
flame-graph tooling with JIT symbolization (`docs/FLAMEGRAPHS.md`, patch 0019).

## 1. "Decompile to C++ + AI intent rewrite" instead of dynamic recompilation

**Verdict: no as a general strategy; yes in a narrow, per-function form.**

Premise-check first: RPCS3's recompilation cost is already amortized. The PPU
LLVM recompiler is effectively AOT — modules compiled once, cached (Skate:
503 s cold, 0 modules warm). SPU compilation is block-level on demand, also
cached. **Nothing is being recompiled during gameplay**; the steady-state cost
is the *quality and semantics* of translated code, not translation itself.

Where the cycles actually go (PROFILE-doa5, AYN Thor):

- 58.3% executing JIT'd guest code — real game work plus emulation-semantics
  overhead (big-endian, 128-bit vectors, xfloat accuracy, reservations).
- ~41% of emulator-side time in `vm::writer_lock` — six SPU threads contending
  on guest memory reservations, i.e. the Cell memory model being emulated
  faithfully. A C++ rewrite of game code does not remove this unless it changes
  synchronization semantics — exactly the change that can't be trusted.

Three walls for the whole-game rewrite:

1. **No correctness oracle.** Matching decompilation projects (SM64, Zelda 64)
   are trustworthy because output recompiles to byte-identical code — and take
   person-years per title. XenonRecomp/Unleashed Recompiled (Xbox 360, same PPC
   family) emits *instruction-accurate* C++, not intent-level, and still needed
   a dedicated team's per-game patching. An AI "intent rewrite" has neither
   oracle; the only test is "the game seems to work," across every code path.
   The Skate `Approximate`-xfloat green-screen shows how sensitive this code is.
2. **Code discovery.** SPU code — the hot part — is data: uploaded to local
   store at runtime, overlaid mid-frame, generated/relocated by job managers
   (SPURS). It cannot be enumerated statically. Skate's `vp6_spu` is exactly
   this.
3. **Distribution.** AI-rewritten decompiled game code is a derivative work of
   the game — a legally grey per-game port, incompatible with this project's
   posture. Emulator + small patches is the established safe shape.

The viable narrow form: **profile-guided, function-level replacement** with a
recorded-I/O oracle — see §4 Tier 2.

## 2. SPU HLE — what's possible

SPU code splits into three categories:

1. **SDK media codecs behind PPU APIs** (`cellAdec`/`cellVdec`/`cellDmux`) —
   already HLE'd to ffmpeg upstream; zero remaining gain. (Skate's VP6 hurts
   because the game drives `vp6_spu` from its own job manager — no API
   boundary.)
2. **The SPURS runtime** — the prize. Sony's scheduler kernel resident in each
   SPU's local store, polling shared workload queues via `GETLLAR` loops and
   DMAing job contexts. This polling/context traffic is a large share of the
   `writer_lock` contention. HLE'ing the scheduler doesn't make that traffic
   cheaper — it **removes** it: workload selection becomes host-side, SPU
   threads run guest code only when a job is assigned, spin-heat (thermal
   throttling feedback) disappears.
3. **Game job payloads** — per-game code, not HLE-able in general; the bulk of
   the 58.3%. Stays emulated regardless.

### Dormant code discovered in the submodule

- `rpcs3/Emu/Cell/Modules/cellSpursSpu.cpp` (~2,100 lines): a substantial HLE
  reimplementation of the SPU-side SPURS kernel — kernel1 *and* kernel2
  workload selection, the system-service workload, and the full taskset policy
  module (task dispatch, context save/restore, SPURS syscalls) — operating on
  real guest-memory SPURS structures so PPU-side code sees consistent state.
- It is **disconnected**: the hook is commented out at `cellSpurs.cpp:1265`
  (`//spu.RegisterHleFunction(entry, spursKernelEntry);`) and
  `RegisterHleFunction` no longer exists on `spu_thread` — the mechanism to
  divert execution at an LS address to a host function was removed years ago.
  Mid-2010s code; upstream abandoned it because LLE became accurate, HLE must
  byte-match every SDK version's statically-linked kernel, and desktop CPUs
  made spinning SPUs free. On an 8-core phone with a thermal budget that
  calculus reverses.
- **Detection infrastructure exists**: `sys_spu.cpp:203-225` SHA1-hashes every
  loaded SPU image and runs the patch engine against `SPU-<sha1>` and
  `<TITLEID>-SPU-<sha1>` keys. "This group just loaded SPURS kernel build X"
  is a hash lookup — the right gate for a per-known-version HLE intercept.

### Relevant existing config knobs (found in `system_config.h`)

- `Max SPURS Threads` (`max_spurs_threads`, default 6, self-described "HACK",
  enforced at `SPUThread.cpp:5806`) — caps running SPURS threads per group.
  A direct dial on the profiled contention.
- `Preferred SPU Threads` (`preferred_spu_threads`) — caps concurrent SPU
  execution.
- `SPU Profiler` (`spu_prof`) — samples SPU program counters, dumps
  time-per-block by hash.

## 3. Expected gain from Sony-library (= SPURS) HLE

"HLE the Sony libraries" collapses, performance-wise, to the SPU-side SPURS
scheduler: media codecs are done, PPU-side wrappers are thin shims over
already-HLE'd lv2, libgcm is direct FIFO writes.

Cycle arithmetic (DOA5/Thor profile): recoverable ≈ `writer_lock` spin (~11% of
total) + scheduler-attributable MFC/DMA (~2%) + **X** (the SPURS kernel's own
guest-code cycles, buried unsymbolized in the 58.3%) + some kernel time →
**~15-30% of total CPU cycles**, X being the big unknown.

Cycles ≠ FPS. The affinity experiment (patch 0017) proved CPU availability is
not the binding constraint — spin cycles reclaimed ≠ frames. The FPS upside is
the *serialization disappearing* (fewer contenders on the range locks, shorter
job-dispatch latency), which the WFE experiment could not deliver (it only made
waiting cheaper; "the threads genuinely cannot proceed").

| Scenario | FPS effect (DOA5-class title) | Notes |
|---|---|---|
| Pessimistic | ~0-5% | but real thermal/battery win → sustained clocks in long sessions |
| Central | **10-20%** (27.8-29.5 avg → ~31-35) | contention moderately on critical path |
| Optimistic | 25-40% | X large and squarely critical-path; don't plan on it |

Hard ceiling: game jobs stay emulated — this is not a path to 60 fps. Internal
~40 fps (breaking the 30-fps vsync quantization line) plausibly is.

### Decisive cheap experiments (do these before any HLE work)

1. **`Max SPURS Threads` A/B at 6/5/4/3** on the DOA5 attract-mode scene —
   free, today. FPS rising with fewer threads proves contention is
   critical-path and sets a *floor* on the HLE win; flat/falling confirms the
   pessimistic case (HLE becomes a thermal project).
2. **`spu_prof` run** during the same fight — measures X by splitting the
   58.3% into SPURS-kernel blocks vs job blocks. (The flame-graph tooling in
   `docs/FLAMEGRAPHS.md` now answers the same question host-side.)
3. **Attribute `writer_lock` acquisitions** (small logging patch): kernel-poll
   vs job-payload DMA.

## 4. AI-generated per-game optimizations

Most tractable of the ideas discussed: the action space is well-defined and
every proposal is machine-verifiable. Three tiers by risk:

- **Tier 1 — per-game config search.** Automate the manual loop that produced
  Skate's per-game config (Debug Console Mode / xfloat Accurate / Strict
  Rendering). The AI's real contribution is *diagnosis* from logs (the session
  templates: `GetLargestFreeBlock = 2552` + AV at `0xfffffffc` → guest OOM →
  Debug Console Mode; green frozen frame + 5 spinning SPUs → xfloat; VK/GCM
  format warnings → strict rendering). Shortcut: ingest the RPCS3 community
  wiki's per-game settings as priors, then device-tune the Android deltas.
- **Tier 2 — AI-generated guest patches** via the existing patch engine
  (`Utilities/bin_patch.h`: writes, `jump`/`jump_link`/`jump_func`,
  `code_alloc`, keyed by PPU hash or `SPU-<sha1>`; per-title YAML — the
  community-patch format, legally unremarkable). Pipeline: profile → hot guest
  function → Ghidra decompile → AI proposes patch (defang idle-poll loop, skip
  redundant work) → verify. **Key asset:** an SPU job is nearly a pure function
  (DMA in → compute → DMA out), so recorded local-store input/output pairs form
  a real oracle — no trust in the AI required.
- **Tier 3 — per-game emulator-path selection** (e.g. "this image hash never
  issues cross-SPU reservations mid-job → relaxed DMA path for it"). Biggest
  wins (attacks `writer_lock` directly), subtlest wrongness; last, and leaning
  hardest on the record/replay oracle.

Build order: the **device-in-the-loop harness is the whole game** — the AI is
replaceable, the oracle isn't. Push config → intent-launch → reach a
repeatable scene (DOA5 attract mode; savestates) → SurfaceFlinger stats +
screenshot + log → emit a BENCHMARKS-style row. Desktop rpcs3 on the same core
version serves as the reference renderer for correctness diffs (the method
that proved the Skate ground artifact was Adreno/Turnip-specific). Guard
against measurement noise: ±1.5 fps run-to-run spread and thermal state are
real; cooldown gates + repeated runs, or the optimizer overfits to throttling.

## 5. Shipped this session: flame graphs with guest symbols

The blocker for all profiling above was the 58.3% anonymous-JIT blob. Fixed —
see `docs/FLAMEGRAPHS.md` for the workflow. Commit `e2e3c81`:

- **Patch 0019** (`patches/0019-jit-perf-map-export.patch`): resurrects the
  `#if 0` perf-map writer in `Utilities/JITASM.cpp` behind `RPCS3_PERF_MAP_DIR`
  — `jit_announce()` already sees every JIT'd function's address/size/name
  (`JITLLVM.cpp:214`, `SPUASMJITRecompiler.cpp:900`). Env re-checked while
  unopened (static-init announces precede the bridge's `setenv`); file kept on
  exit for post-mortem symbolization; shutdown sentinel no longer logs an
  error. Upstream-friendly: with `/tmp`, desktop `perf` finds it natively.
- **Bridge**: `adb shell setprop debug.cellstation.perfmap 1` →
  `RPCS3_PERF_MAP_DIR=<root>/cache`. No UI, no rebuild.
- **`tools/flamegraph.py`**: folds `perf.data` via the NDK's
  `simpleperf_report_lib`, substituting JIT-map names; emits folded stacks for
  speedscope / `flamegraph.pl`; per-thread prefixes and `--thread` filtering
  (SPURS threads carry the game's `…CellSpursKernel<n>` names).
- Verified: series 0001-0018 applies cleanly to the pinned SHA; map
  parsing/lookup unit-tested. **Not yet compiled for Android or run against a
  device capture** — first on-device run is the integration test. Expected
  limitation: guest frames are flat (no unwinding through JIT code) —
  attribution by function works, guest→guest call trees don't.

## 6. Ranked next actions

1. `Max SPURS Threads` 6/5/4/3 A/B on DOA5 attract mode (config only, ~an hour
   with existing methodology).
2. First on-device flame-graph capture (validates patch 0019 + tooling, and
   measures X = SPURS-kernel share of guest cycles in the same session).
3. `spu_prof` run as cross-check on X.
4. Decide on SPURS HLE investment from (1)-(3): any positive movement in (1)
   makes it the highest-leverage project on the roadmap; flat + small X shelves
   it as a thermal-only play.
5. Tier-1 harness (config search automation) — independent of the HLE
   decision, reuses the same measurement plumbing.
6. Range-lock granularity investigation (systemic fix, already flagged in
   PROFILE-doa5) remains open in parallel.
