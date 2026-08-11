# SPU HLE candidate survey — DOA5U in-fight profile

Ranking of guest SPU code by measured CPU cost, classified from disassembly and
attributed to source images, to decide what to HLE next with the pipeline in
`docs/SPU-HLE-PIPELINE.md` (strategy: `docs/RESEARCH-ai-optimization.md` §2/§4).

## Data and method

| input | what |
|---|---|
| `fgfight/fight-stock.folded` | in-fight CPU profile (stock build), folded stacks; guest SPU blocks appear as `__spu-cx<LS>-<hash>` leaves |
| `spu.log` | SPUDisAsm dump of 4,104 SPU blocks (separate SPU-Debug session, boot→menu→fight attempt) |
| `spuelf/spu_progs/` | 25 SPU ELFs dumped at `sys_spu_image::deploy`, with real SPUNAMEs |

Method: aggregate profile cost by `__spu-cx` leaf → slice each hot block's
disassembly from `spu.log` by (LS address, hash) → classify by instruction mix →
attribute to an image by searching every dumped ELF for the block's
**per-instruction top-byte string** (invariant under PIC relocation, unlike raw
bytes, since only immediate fields of `il/ila/br…` change). "Resident" =
matched at the same LS address it executed at; "~image" = code found elsewhere
in that image (relocated or shared runtime). Analysis script:
`scratchpad/hle_survey2.py`, machine-readable rows in `scratchpad/hle_rows.json`.

## Topline numbers

- Total period: 92.68 G samples-cycles. **SPU threads = 68.5%** of it.
- Guest SPU code (all `__spu-cx` leaves): **39.9%** across 470 distinct sampled blocks.
- Biggest host-side costs on SPU threads: `vm::writer_lock` **11.1%**,
  `process_mfc_cmd` 3.3%, `do_dma_transfer`+`do_list_transfer` 1.3%.

### Thread map (who runs what)

| threads | share of total | identity |
|---|---|---|
| `SPU[0x0000100]`, `SPU[0x1000100]`–`SPU[0x4000100]` | **56.6%** (5 × ~11%) | main SPURS group, kernel2 (`03e32a2c…`); runs the game's job/task code |
| `SPU[0x0000200]` | **12.7%** | dedicated audio stack: mstream DSP jobs at LS 0x80+, ATRAC decoder, mixer glue |
| PPU threads | ~20% | GCM/main/audio-mixer etc. (out of scope here) |

### Guest cost by region/source

| % of total | what |
|---|---|
| 14.3% | MAIN group, **fight-only job code** — not present in the spu.log dump session (no disassembly available; see caveat) |
| 12.6% | MAIN group, job code in spu.log but **in no dumped ELF** → dynamically DMA'd job/overlay code |
| 3.25% | MAIN group, `Rel(gamelib)-6e2594` — game's resident SPU job-executor task (linked+running at 0x3000) |
| 1.2% | MAIN group, policy-module region (0xa00–0x3000) |
| 0.16% | MAIN group, SPURS kernel region (<0xa00) — the kernel barely shows as guest cycles; its real cost is the MFC/reservation traffic it generates (`writer_lock` 11.1% is host-side) |
| 4.4% | AUD thread, mstream DSP chain (i3dl2 reverb 3.6%, para_eq 0.6%, filter 0.1%, meter 0.03%) |
| 2.9% | AUD thread, `msngSPURS_ATRAC` code (matched relocated) |
| 2.2% | AUD thread, unknown high-LS code (0x34000+; likely more audio-stack/mixer code, absent from dump) |

## Ranked hot blocks (top 25 of 470; 32.9% of total in top 40)

grp: MAIN = 5-worker SPURS group, AUD = audio thread. src: `exact` = disasm from
spu.log; `slot-hint` = different code occupied that LS slot in the dump session
(weak evidence); `?` = no disassembly exists.

| # | % total | LS | insns | grp | class (mix) | image | reading |
|---|---|---|---|---|---|---|---|
| 1 | 3.07 | 0xcf24 | ? | MAIN | ? (slot-hint: DMA glue, wrch×24, mpyh) | — | job stage w/ heavy DMA-list building |
| 2 | 2.99 | 0xa538 | ? | MAIN | ? (slot-hint: 23-insn prologue stub) | — | tiny hot function, likely called per-element |
| 3 | 2.83 | 0x5498 | 173 | MAIN | DMA/channel glue (il×18, wrch×8, full MFC GET/PUT setup) | — (dynamic job) | job prologue: DMA in/out + tag wait |
| 4 | 2.64 | 0x9438 | 247 | MAIN | float SIMD (shufb×73, fma×28, fcgt) | — (dynamic job) | **vertex/skinning-type transform kernel** (AoS↔SoA shuffles + FMA) |
| 5 | 2.10 | 0xf54c | ? | MAIN | ? fight-only | — | |
| 6 | 1.73 | 0xe5f0 | 510 | MAIN | mixed float (shufb×86, fma×38, fm/fms) | — (dynamic job) | second transform kernel, same family as #4 |
| 7 | 1.45 | 0x4f8 | 351 | AUD | int+shuffle, some fma | **mstream_dsp_i3dl2** (resident) | I3DL2 reverb inner loop |
| 8 | 1.38 | 0xa268 | 549 | MAIN | integer/control (mr×97, shufb×64, a×56) | — (dynamic job) | data marshaling / index processing |
| 9 | 1.05 | 0xfbf0 | ? | MAIN | ? fight-only | — | |
| 10 | 0.92 | 0xa78 | 137 | AUD | mixed float+int | mstream_dsp_i3dl2 (resident) | reverb |
| 11 | 0.90 | 0xf368 | ? | MAIN | ? fight-only | — | |
| 12 | 0.86 | 0x36bb4 | ? | AUD | ? fight-only | — | audio-stack high-LS code |
| 13 | 0.84 | 0x5bd0 | 191 | MAIN | float SIMD (shufb, fm/fma, **frsqest/fi**) | — (dynamic job) | normalization/rsqrt kernel (normals or constraint solve) |
| 14 | 0.83 | 0xfb08 | ? | MAIN | ? fight-only | — | |
| 15 | 0.75 | 0x4de8 | 224 | AUD | float SIMD (fa/fm 38%) | ~msngSPURS_ATRAC | ATRAC filterbank/MDCT-type math |
| 16 | 0.64 | 0x3aa0 | 11 | MAIN | **dispatch-loop tail** (counter++ mod 4, `br 0x392c`) | Rel(gamelib)-6e2594 (resident) | job-executor loop bookkeeping |
| 17 | 0.60 | 0xf8 | 182 | AUD | integer/control | mstream_dsp_para_eq (resident) | shared mstream job runtime stub (same code heads filter.pic) |
| 18 | 0.55 | 0x188 | 166 | AUD | float SIMD | mstream_dsp_i3dl2 (resident) | reverb |
| 19 | 0.52 | 0x3a20 | 32 | MAIN | table-driven dispatch, ends `bisl lr,r65` | Rel(gamelib)-6e2594 (resident) | **indirect call through handler table at LS 0x5450** |
| 20 | 0.43 | 0x9c60 | 323 | AUD | DMA/channel glue | ~msngSPURS_ATRAC | decoder DMA/state machine |
| 21 | 0.43 | 0x343c0 | ? | AUD | ? fight-only | — | |
| 22 | 0.41 | 0x901c | ? | MAIN | ? (slot-hint 4-insn) | — | |
| 23 | 0.41 | 0x34a30 | ? | AUD | ? fight-only | — | |
| 24 | 0.38 | 0x9d58 | ? | MAIN | ? (slot-hint: frsqest kernel) | — | rsqrt family |
| 25 | 0.37 | 0x420 | 54 | AUD | mixed float (shufb 48%) | mstream_dsp_i3dl2 (resident) | reverb |

Remaining Rel-6e2594 blocks (0x3198/0x3228/0x3348/0x3900/0x392c/0x3414/0x3ce0/0x435c,
each 0.09–0.34%) are all small integer/control/DMA blocks of the same dispatch
loop — together the executor's resident code is ~3.25%, of which roughly ~1.5–2%
is dispatch/poll overhead rather than payload.

## Image census — Sony/middleware vs game code

| image | origin | in-fight cost | notes |
|---|---|---|---|
| `spurs_kernel2` (03e32a…) | Sony (SPURS) | 0.16% guest (+drives 11.1% host `writer_lock`) | patch 0020 HLE target; win is contention removal, not these cycles |
| `spurs_kernel` v1 (dd4f97…) | Sony | not sampled | second SPURS instance exists but idle in-fight |
| `mstream_dsp_i3dl2/para_eq/filter/meter` (.pic) | Sony (MultiStream DSP plugins) | 4.4% | run at LS 0x80+ on the audio thread; hashes already in the pipeline table |
| `msngSPURS_ATRAC` (702a72…) | Sony (MultiStream) | 2.9% | ATRAC3+ decode; code observed running relocated (matched fuzzily, non-resident) |
| `at3dec` (91c6f1…) | Sony | 0 in-fight | menu/BGM path presumably |
| `avcdec_spu.elf`, `init_avc.elf` | Sony (cellVdec AVC) | 0.05% | cutscene-only |
| `PS3SPUIoZlib`, `PS3SPUGraphSWC` | Koei Tecmo shared SPU libs | ~0.01% | load-time / trace only |
| `Rel…69d36b` (36KB) | **Sony EDGE 1.2.3.0** — strings: "1.2.3.0-PS3-SPU-EDGE", edgeZlib error text | 0.03% | EDGE Zlib inflate task; load-time decompression, idle in-fight — *proves the game links EDGE* |
| `Rel…6e2594` (10KB) | game lib (Team Ninja `DOA5U_master\libs\lib\ps3\Rel`) | **3.25%** | resident SPURS-task job executor: fetch-command → table dispatch (`bisl` via LS 0x5450 table) → loop; loads/dispatches the dynamic job code below |
| `thread_a, a1, b1–b4, c1–c3, d1, d2` | game-side **H.264/JVT movie decoder pipeline** (thread_a spawns "vcl" SPURS tasks; a1 = NAL/CABAC/deblock config, jvt_assert; b2 = scan/quant tables; c* = prediction/deblock, "baseline/high444" profile strings) | **0 in-fight** | movie playback only (intro/story). Not worth HLE for gameplay perf |
| *(no image)* dynamic job code | game jobs, DMA'd by the executor / SPURS | **~27%** (12.6% disassembled + 14.3% fight-only) | the actual per-frame workload: skinning/normal/physics-type SIMD kernels + their DMA glue |

## Ranked HLE candidates

### 1. Audio stack: mstream DSP chain — 4.4% guest (up to ~6% with its share of MFC/host)
**Feasibility: HIGH — already the pipeline's first target; this data confirms it.**
i3dl2 reverb alone is 3.6%; para_eq 0.6%; filter/meter noise. Small resident
.pic jobs with known descriptor layout (`docs/spurs/dsp-descriptors.txt`), full
disassembly available, hashes in the pipeline table. Replacement: host
reverb/biquad kernels, or passthrough for measurement. Must honor the
self-managed GET→process→PUT contract (deadlock gotcha in the pipeline doc).
This also directly attacks the audio thread (12.7% of CPU) which is one of only
6 SPU threads — freeing most of a core.

### 2. msngSPURS_ATRAC — 2.9% guest
**Feasibility: HIGH (decoder) / MEDIUM (integration).** ATRAC3+ → ffmpeg host
decode, the "continuous audio prize" from the pipeline doc. Complication found
here: its code executed at non-linked addresses (relocated), so the intercept
should key on image hash at deploy + entry offset, not a fixed LS address.
Together with #1 this is the whole audio thread: **~9.4% guest, 12.7% thread
total** — the cleanest large win available, all Sony/middleware code.

### 3. SPURS kernel2 + taskset policy (patch 0020) — 1.4% guest, gates 11.1% `writer_lock`
**Feasibility: IN PROGRESS.** The survey confirms the strategy doc's prediction:
the kernel's own guest cycles are trivial (0.16%); the value is removing the
GETLLAR/PUTLLC polling that produces the 11.1% host-side `writer_lock` spin and
3.3% `process_mfc_cmd` on the 5 workers. Keep going, but measure with the
contention metric, not guest-leaf cost.

### 4. Rel-6e2594 job-executor task — 3.25%, of which ~1.5–2% is dispatch/poll overhead
**Feasibility: MEDIUM, and better served by a targeted patch than full HLE.**
Game code, but: a small (10KB) stable resident image, dumped, stable hash, and
its hot blocks are a fetch/dispatch loop (0x392c–0x3ce0) around `bisl` handler
calls. Options: (a) HLE only the dispatch loop (host-side fetch + LS call into
guest handlers — a miniature of the SPURS-kernel approach); (b) patch-engine
defang of the poll when queues are empty. Payload handlers stay guest. The
recorded-I/O oracle applies cleanly since the executor's queue protocol is
observable in LS.

### 5. Dynamic game job kernels — the ~27% elephant
**Feasibility: LOW–MEDIUM per job; highest ceiling.** The bulk of the fight
workload: SIMD transform kernels (0x9438 2.64%, 0xe5f0 1.73%, 0x5bd0 0.84%,
0xc4d8/0x5d88 0.5% — shufb+fma+frsqest = skinning / normal recompute /
constraint math) plus their DMA glue (0x5498 2.83%, 0xcf24 3.07%) and
marshaling (0xa268 1.38%). Not in any deployed ELF — DMA'd by the executor
task (#4), so interception must hook job-code upload (DMA write into LS +
entry), not image deploy. The game links EDGE (see census), so some of this may
be EDGE Geom-derived skinning — worth checking the job binaries against EDGE
Geom reference code before reverse-engineering from scratch. Start with the
two biggest float kernels (#4+#6 ≈ 4.4%): near-pure DMA-in→compute→DMA-out
functions, ideal oracle targets.

**Not candidates:** thread_a–d2 movie pipeline (0 in-fight), EDGE Zlib /
PS3SPUIoZlib (load-time), avcdec (cutscenes), PS3SPUGraphSWC (trace-only).

## Caveats and immediate actions

1. **14.3% of total CPU is fight-only MAIN-group job code with no disassembly**
   (plus 2.2% on the audio thread): the spu.log dump session never compiled
   those exact blocks (hashes absent). Before any HLE of category 5, **re-dump
   spu.log during an actual fight** (SPU Debug on, enter versus, then grab the
   log) to close the gap. Blocks #1/#2/#5/#9/#11/#14 in the table — 10.9%
   combined — are in this bucket.
2. Leaf self-cost understates true block cost: MFC work appears as host frames
   (`exec_mfc_cmd→…→writer_lock`) without an interposed guest frame, so the
   11.1% writer_lock cannot be attributed to specific blocks from this profile.
3. Same-slot hints (#1, #2, #24) are weak evidence — the LS slot held different
   code in the dump session; treat the classification as suggestive only.
4. Percentages are of the whole profile period (all threads); multiply by ~1.46
   to express as share of SPU-thread time.

*Generated from `scratchpad/hle_survey2.py`; rerun against any new .folded +
spu.log pair.*

## Addendum: fight-time SPU capture (2026-08-12)

Re-dumped spu.log during an actual Arcade fight (Kasumi vs Sarah/Sabin, Stage 01)
— SPU cache cleared so all fight-only kernels recompiled and dumped.
**4874 blocks vs 4104 at the title (+770 fight-only kernels)**, incl. the two
hottest leaves in the whole profile that were previously missing. Fight disasm
archived: `docs/spurs/doa5u-spu-fight-disasm.log.gz`.

Top in-fight guest SPU kernels, now classified from their instruction mix:

| kernel (LS/hash) | % total CPU | class |
|---|---|---|
| 0x0cf24 ViWM… | 3.07% | mixed int + fp15 shuf17 |
| 0x0a538 bfyc… | 2.99% | **FLOAT-SIMD 21% — skinning/geom/physics** |
| 0x05498 VqbX… | 2.83% | shuffle-heavy int (fp0 shuf17) — data marshaling |
| 0x09438 JGKH… | 2.64% | **shuffle-heavy 29%** — geometry reshaping |
| 0x0f54c svmb… | 2.10% | mixed |
| 0x0e5f0 XVar… | 1.73% | **FLOAT-SIMD 18% — skinning** |
| 0x004f8 6uuR… | 1.45% | DMA/channel — (i3dl2 reverb, audio) |
| 0x36bb4 rKrR… | 0.86% | **FLOAT-SIMD 26% — skinning** |

Top 6 kernels ≈ 15% of all CPU. Shape = **character skinning/geometry/physics
(float-SIMD) + geometry data reshaping (shuffle-heavy)** — i.e. DOA5's per-frame
graphics job pipeline on the SPUs. Consistent with Sony **EDGE Geometry** (the
game links EDGE — `edgeZlib` found in the Rel…69d36b image), which does exactly
this: SPU skinning, culling, vertex decompression feeding the RSX.

**HLE feasibility of this frontier is LOW compared to audio.** These kernels are
per-frame, tightly coupled to the RSX (they produce vertex data the GPU
consumes), and are NOT pure functions with simple I/O — the recorded-I/O oracle
is harder to apply. Replacing them means a host skinning/geometry pass wired into
the RSX vertex path — a large, GPU-coupled project. If they are stock EDGE, a
known reference implementation exists, which is the only thing that makes it
tractable; verify EDGE provenance before any RE.

**Revised strategic ranking:**
1. **Audio (DSP + ATRAC) ≈ 12.7% / ~one core** — pure functions, host libs
   exist, oracle-verifiable. Cleanest win. DSP bypass already shipped (patch
   0020); ATRAC→ffmpeg next.
2. **SPURS scheduler** — small guest cost but gates ~11% writer_lock contention;
   systemic. Finish patch 0020.
3. **EDGE geometry/skinning ≈ 15-27%** — biggest raw cost but hardest (GPU-
   coupled, per-frame). Only worth it if confirmed stock EDGE with a reference
   impl; otherwise a research project, not an optimization.
