# DOA5U in-fight SPU: EDGE provenance, reimplementation cost, and the IR→C++ idea

Companion to `docs/spurs/hle-candidate-survey.md` (fight addendum) and
`docs/SPU-HLE-PIPELINE.md`. The survey found that ~15–27% of all CPU in an
Arcade fight is a cluster of character skinning / geometry / physics SPU
kernels, and flagged them as "consistent with Sony EDGE Geometry." This note
(a) pins what is and isn't actually EDGE, (b) estimates the cost of a host
reimplementation, and (c) evaluates — with a small compile experiment — the
proposal to mechanically transpile rpcs3's LLVM IR for a kernel into C++ and run
it unmodified.

Inputs: the 25 dumped SPU ELFs (`scratchpad/spuelf/spu_progs/`, real SPUNAMEs),
the fight-time SPUDisAsm dump (`docs/spurs/doa5u-spu-fight-disasm.log.gz`, 4874
blocks), the profile (`scratchpad/fgfight/fight-stock.folded`), and rpcs3's SPU
recompiler source (`SPULLVMRecompiler.cpp`, pinned submodule).

---

## Task A — EDGE provenance and version

### What the strings actually prove

Every dumped image was swept for EDGE / Sony / version / geometry markers
(`scratchpad/edge_grep*.sh`). Exactly **one** image carries EDGE branding:

| image (SPUNAME) | hash (prefix) | EDGE evidence |
|---|---|---|
| `…\DOA5U_master\libs\lib\ps3\Rel` (36 KB) | `69d36b1c…` | `1.2.3.0-PS3-SPU-EDGE`; `edgeZlibInflateRawData`, `edgeZlibFetchAndInflateLargeRawData`, `EDGE ZLIB ERROR: …`, `EDGE ASSERTION FAILURE` |
| `PS3SPUIoZlib` | `941db4cd…` | `inflate 1.2.3 Copyright 1995-2005 Mark Adler` (plain zlib, the wrapped codec) |

That is the whole of the EDGE evidence. Concretely:

- **The game links EDGE Zlib**, the decompression component of Sony's
  "PlayStation Edge" (a.k.a. Edge Tools) SPU library suite. Confirmed by hard
  branding strings, a `.sceversion` ELF section (Sony toolchain), and the
  matching plain-zlib `PS3SPUIoZlib` it wraps.
- **The version string pins the Zlib component, not the EDGE suite.**
  "1.2.3.0-PS3-SPU-EDGE" tracks **zlib 1.2.3** (the same 1.2.3 Mark Adler build
  seen in `PS3SPUIoZlib`), i.e. it is EDGE Zlib's build tag on top of upstream
  zlib 1.2.3. It does **not** independently reveal the EDGE Geometry / EDGE SDK
  bundle revision. Treat "EDGE 1.2.3.0" as *EDGE Zlib on zlib 1.2.3*, dated to
  the PS3 SDK 3.x era, and no more.
- **No EDGE Geometry, Edge Animation, Edge Post, or Edge Physics string or
  symbol exists in any dumped image.** A targeted sweep for `edgeGeom`,
  `PamGeometry`, `edgeAnim`, `blendshape`, `skinMatrix`, `skinning`, etc. across
  all 25 ELFs returns nothing.

### The census (who is Sony/middleware vs Team Ninja)

| image | origin | in-fight cost | note |
|---|---|---|---|
| `Rel…69d36b` | **Sony EDGE Zlib** (zlib 1.2.3) | ~0.03% | load-time decompression; idle in-fight. *The only proof the game links any EDGE.* |
| `PS3SPUIoZlib`, `PS3SPUGraphSWC` | Koei Tecmo shared SPU libs (`SPU_Release2\…`) | ~0.01% | GraphSWC is stripped, name only; trace/load-time |
| `mstream_dsp_*` (.pic), `msngSPURS_ATRAC`, `at3dec` | Sony MultiStream / ATRAC | 7.3% (fight) | audio stack — separate HLE track |
| `avcdec_spu`, `init_avc`, `thread_a…d2` | Sony cellVdec + game's **H.264 movie decoder** (thread_a: `create_vcl_tasks`, profiles `baseline/high/high10/high422/high444`) | 0% in-fight | cutscene/movie only |
| `spurs_kernel2` / `spurs_kernel` | Sony SPURS | 0.16% guest | patch 0020 target |
| `Rel…6e2594` (10 KB) | **Team Ninja game lib** (`DOA5U_master\libs\…\Rel`, no EDGE strings) | 3.25% | resident SPURS-task job executor: fetch → table-`bisl` dispatch → loop |
| *dynamic job code* (no image) | **game jobs, DMA'd by the executor** | **~27%** | the hot skinning/geometry/physics kernels — see below |

### The hot kernels are not in any ELF

The kernels that dominate the fight — LS `0x0a538`, `0x0cf24`, `0x09438`,
`0x0e5f0`, `0x0f54c`, `0x36bb4`, `0x05498` — appear **only** as recompiled
blocks in the fight disasm, in **no** dumped ELF. They are uploaded to local
store at runtime by the `Rel…6e2594` executor (DMA-in job code), which is why
they cannot be hash-matched to a known binary. So provenance for *these* kernels
can only be argued from structure, not from a string or a hash.

### Structure → EDGE-stage mapping (instruction mix from the fight disasm)

Instruction histograms per kernel (`scratchpad/slice.py`):

| kernel | insns | dominant mix | reading (EDGE-stage analogue) |
|---|---|---|---|
| `0x0f54c` (2.10%) | 295 | shufb 64, fma 32, fm 26, fms 12, **cuflt 4** | **decompress input verts** (`cuflt` = uint→float dequantize) **→ transform** |
| `0x0e5f0` (1.73%) | 510 | shufb 86, lqd 62, **fma 38**, fm 26, fms 12 | **skinning / vertex transform** (matrix MAC + AoS↔SoA reshaping) |
| `0x36bb4` (0.86%) | 527 | lqd 75, shufb 75, **fma 56**, fa/fs 32, **cflts 8** | **skin → requantize output** (`cflts` = float→int pack for RSX vertex format) |
| `0x0cf24` (3.07%) | 185 | **lqx 32** (gather), shufb 25, selb 25, **frsqest 4, fi 4** | **normalize / cull**: indexed vertex gather + rsqrt-normalize + `selb` compaction (normals or backface/frustum cull) |
| `0x0a538` (2.99%) | 321 | mr 42, shufb 35, **lqx 19** (gather), and 13, roti/rotqbyi 17, cflts 6 | **index processing / de-indexing / gather** setup for the above |
| `0x05498` (2.83%) | small+loop | lqd/stqd, `brsl`, MFC | **DMA glue / job prologue** feeding the kernels |

The `0x0cf24` body (dumped in full) is textbook indexed geometry: it computes an
index (`mpya`), gathers eight quadwords per iteration via `lqx r,ra,ridx`,
`shufb`-reassembles them, then rsqrt-normalizes (`frsqest`+`fi`). That
decompress → skin → normalize/cull → requantize shape is exactly what EDGE
Geometry does on the SPU, feeding vertex buffers to the RSX.

### Verdict on provenance

- **Proven:** DOA5U links Sony's EDGE suite (Zlib component, zlib-1.2.3 vintage,
  SDK 3.x era). Team Ninja shipped their own SPURS job executor (`Rel…6e2594`)
  and their own H.264 movie pipeline.
- **Strongly suggested but NOT proven:** the hot skinning/geometry kernels are
  EDGE Geometry. Evidence is circumstantial (they ship the EDGE suite) plus
  structural (the pipeline shape matches). There is **no** string, symbol, or
  hash tying those specific kernels to EDGE Geometry, because they are dynamic
  job code. They could equally be Team Ninja's in-house skinning built to the
  same well-known recipe. **Do not treat "it's stock EDGE" as established.**
- **Reference implementation, realistically:** EDGE Geometry's algorithms are
  publicly documented (SCE's GDC 2008 "PlayStation Edge" talks; the SPU
  geometry-processing pipeline is described in the literature), and the library
  source shipped in the PS3 SDK **samples**. But that source is Sony
  SDK-licensed / NDA material, **not open-source and not redistributable**. So a
  reference "exists" for *understanding the algorithm*, not as a drop-in you can
  legally vendor. The survey's "open-ish" framing should be tightened to that.

---

## Task B — cost of a host reimplementation

EDGE Geometry produces RSX-ready vertex data: it reads compressed/indexed vertex
streams + skinning matrices from main memory via DMA, decompresses, applies
blend shapes and matrix-palette skinning, culls, requantizes to the RSX vertex
format, and writes the result back to a buffer the RSX then draws from (via a
patched-in draw command / vertex-buffer pointer). A host reimplementation has to
reproduce that whole contract, not just the arithmetic.

### Where it would have to hook

```
game → SPURS executor (Rel…6e2594) DMAs job code + descriptor into LS, bisl
                                   │  (intercept point is HERE, not sys_spu_image::deploy —
                                   │   the kernels never deploy as an image)
                                   ▼
          [SPU job: decompress → skin → cull → requantize → PUT verts]  ← replace this
                                   ▼
      output vertex buffer in main memory ──▶ RSX draws it (GCM command stream)
```

Interception cannot key on an image hash at deploy (the survey's clean mechanism
for audio) because these kernels have no image. It must hook **job-code upload**:
recognize the descriptor/entry the executor is about to `bisl` into and divert to
a host skinning pass. That is a materially weaker and more fragile hook than the
audio path.

### Why this is far harder than the audio HLE

The audio DSP/ATRAC HLE works because those jobs are **oracle-clean pure
functions**: DMA a buffer in, compute, DMA a buffer out; a recorded input→output
pair bit-verifies any replacement (`SPU-HLE-PIPELINE.md` §6). The geometry
kernels break every one of those properties:

1. **Per-frame, on the critical path.** Runs every frame for every visible
   character; latency and RSX synchronization matter, not just throughput.
2. **GPU-coupled, not oracle-clean.** The output is consumed by the RSX in a
   specific quantized vertex layout, referenced by GCM draw commands with
   matching stride/format/offset. "Bit-exact output" now means "byte-exact RSX
   vertex buffer *and* consistent with the command stream that draws it." The
   recorded-I/O oracle still applies in principle (the PUT'd buffer is
   observable) but the surface is much larger and the correctness bar is
   pixel-visible, not silence-in-the-background.
3. **Stateful / data-driven.** Blend-shape weights, matrix palettes, LOD, and
   compression formats vary per model and per frame; a replacement must parse
   EDGE's descriptor formats exactly (segment tables, compression modes, fixed-
   point scales), which are precisely the SDK-licensed details we don't have in
   source.
4. **Discovery is runtime.** Which job is "the skinning job" is only known once
   the executor uploads it; there's no API boundary or stable image hash to gate
   on. (`RESEARCH-ai-optimization.md` §1 wall #2, in its sharpest form.)

### Effort / risk

- **If treated as an optimization (drop-in HLE): not worth it.** Realistically a
  multi-person-month reverse-engineering + host-renderer-integration project
  (reconstruct EDGE descriptor formats → host skinning pass in SoA/NEON or a
  compute shader → wire into the RSX vertex path → per-model correctness pass
  against the desktop reference renderer), with pixel-visible failure modes and
  a fragile job-upload hook. High effort, high risk, and it is **not** a path to
  60 fps because the RSX-side draw work is unchanged.
- **Stock-EDGE provenance would help but does not rescue it.** *If* it were
  confirmed EDGE Geometry *and* we could reference the documented pipeline, the
  RE cost drops (known descriptor layout, known stages). But (i) provenance is
  unproven, (ii) the reference is not legally vendorable, and (iii) the hard part
  — RSX coupling and the runtime hook — is unchanged by knowing the algorithm.

**Recommendation stands with the survey:** audio (pure-function, oracle-clean,
host libs exist) and the SPURS scheduler (systemic contention) are the wins.
The geometry frontier is a research project, not an optimization — and the first
step before *any* RE is to actually confirm EDGE Geometry provenance (e.g. hash
the uploaded job code against an EDGE Geometry reference binary, if one can be
obtained), rather than assume it.

---

## Task C — the "IR → C++, run unmodified" idea

> Could we take rpcs3's LLVM IR for a kernel, convert it to C++, and get it
> working with no modification — and would it be faster?

### How you'd do it, and whether the tooling exists

rpcs3 with `SPU Debug: true` dumps `spu-ir.log` (~135 MB; interpreter template
first, then the recompiled per-block IR). Turning that IR into C++ has three
candidate routes:

1. **`llc -march=c`** (the old LLVM C backend) — **dead.** Removed from LLVM in
   ~3.1 (2012). Confirmed on the host toolchain: `llc -march=c` →
   `invalid target 'c'` (LLVM 21.1.2). rpcs3's vendored LLVM is a modern fork
   (16–19 range), so this route does not exist there either.
2. **`llvm-cbe`** (the out-of-tree resurrected C backend) — **not installed and
   not a fit.** It must be built against the exact LLVM your IR came from, it
   targets older IR/opcode sets and lags current LLVM, and it emits C that is a
   1:1 transliteration of the IR (a `goto`-soup over the same SSA values and the
   same intrinsics). It exists but it is fragile version-coupled tooling, not a
   turnkey path.
3. **Hand / AI translation** of a block — feasible for a *small* block, but this
   is no longer "no modification"; it's the intent-rewrite in disguise.

So even step one — "convert to C++" — is not a clean button on rpcs3's LLVM. The
only mechanical option is `llvm-cbe`, version-matched and hand-held.

### Would it even run?

**No, not standalone.** The recompiler's IR is not free-floating math over
plain arrays. From `SPULLVMRecompiler.cpp`: every chunk is
`define … @chunk(spu_thread* %thr, i8* %ls, i32 %pc, …)`; general-purpose
registers are **GEPs into `spu_thread::gpr[128]`** typed `<4 x i32>`; LS
accesses go through the `%ls` pointer with the `& 0x3FFFF` wrap; blocks **call
sibling chunks and runtime helpers** threading `%thr` through (`CreateCall(callee,
{m_thread, m_lsptr, m_base_pc,…})`), and MFC/DMA/channel ops are calls into
rpcs3's SPU runtime. A transpiled function therefore only runs if you link it
against the real `spu_thread` struct layout, the MFC/DMA implementation, the
reservation/`vm` subsystem, and the chunk-dispatch machinery. It is not a
self-contained C++ function you can drop into a host skinning pass — it *is*
rpcs3's SPU execution, spelled in C instead of IR.

### The key question — is it faster? (experiment)

**No. A mechanical transpile runs at essentially the same speed as the existing
JIT, because it lowers through the same backend to the same native code.** The
profile's "58% JIT'd guest code" already *is* this IR compiled to native ARM64
by rpcs3's LLVM JIT. AOT-compiling the same IR yields the same instructions.

Demonstrated directly (`scratchpad/spu_kernel.ll`, `run_experiment.sh`): I wrote
a faithful mock of an rpcs3-style chunk — an FMA "skinning step" with the real
IR shape (reg file as `<4 x i32>` GEPs into a thread struct, one LS load with the
`& 0x3FFFF` mask and big-endian `bswap`, `fma.v4f32`) — and lowered it with
`llc -O3 -mcpu=apple-m1`:

```asm
; [A] SPU-semantics kernel  (mechanical transpile keeps ALL of this)
ldr   w8, [x0, #496]        ; load index reg from thread struct
and   x8, x8, #0x3ffff      ; SPU LS wrap  ── SEMANTICS TAX
ldr   q0, [x1, x8]          ; LS gather
rev32.16b v0, v0            ; big-endian byteswap ── SEMANTICS TAX
ldr   q1, [x0, #1280]       ; reg a  (memory round-trip through struct)
ldr   q2, [x0, #1296]       ; reg b
ldr   q3, [x0, #1312]       ; reg c
add.4s  v0, v1, v0
fmla.4s v3, v2, v0          ; the actual skinning math
str   q3, [x0, #480]        ; write reg back to struct
ret
```

The FMA lowers to a **single native `fmla.4s`** — exactly what the JIT emits.
The *other* seven instructions are pure SPU semantics: the `& 0x3ffff` LS wrap,
the `rev32.16b` big-endian swap, and the load/store traffic through the register
file in the thread struct. A mechanical IR→C++ transpile preserves every one of
them, because it preserves the IR.

Contrast the **intent rewrite** (`native_skin.ll`) — the same arithmetic done
host-native (SoA float4, no byteswap, no LS, values in registers):

```asm
; [B] native-intent skin
fmla.4s v2, v1, v0
...
ret
```

**8 real instructions vs 1** for the same useful math. The 7:1 gap is the SPU
emulation tax, and it is invisible to the JIT-vs-AOT question — both compilers
keep it. Extrapolate to the real kernels, whose mix is `shufb`-dominated
(`0x0e5f0`: 86 `shufb`): those byte-permutes become NEON `tbl` sequences that
exist *only* because the SPU has a 128-bit-only register file and does AoS↔SoA
reshaping a host algorithm would never do. Mechanical transpile keeps all the
`tbl`s; a host skinning pass has none of them.

### Is there any real AOT-vs-JIT angle?

Minor and second-order, not the 15–27% category win:

- **No warmup / no compile pause:** real, but rpcs3 already caches SPU block
  compilation, so steady-state gameplay isn't recompiling. Marginal.
- **Whole-function `-O3` / LTO / PGO offline:** an AOT pass can inline across the
  chunk boundaries the JIT compiles semi-independently and spend optimization
  time the JIT won't. Plausibly a modest constant-factor (order ~5–20% on some
  blocks), from better register allocation and cross-block scheduling — worth
  something, but it does not touch the semantics tax that dominates these
  kernels.
- Everything else (dispatch overhead, "native vs interpreted") does **not**
  apply: the JIT is already native.

### Verdict on the IR→C++ idea

- **Viable as stated ("no modification, faster")? No.** The only mechanical tool
  (`llvm-cbe`) is dead-adjacent/version-fragile; the output won't run standalone
  (it needs rpcs3's `spu_thread` + MFC/DMA/dispatch runtime); and it would run at
  ~JIT speed because it lowers through the same LLVM backend to the same NEON.
  The overhead is **SPU semantics** (128-bit vectors, `shufb`/`tbl` reshaping,
  big-endian, LS load/store model, DMA), not instruction dispatch — and a
  mechanical transpile preserves all of it by construction.
- **What it's actually good for:** a **reverse-engineering / readability aid.**
  C is easier to read and annotate than 500 lines of SPU asm or LLVM IR, so an
  IR→C rendering (or better, the disasm + IR read the pipeline doc already
  prescribes) helps a human *understand* a kernel on the road to an
  intent-level host rewrite. That understanding is the valuable output — not the
  transpiled code itself.
- **Where the speed actually comes from:** recognizing "this is skinning /
  vertex decompression / culling" and doing it **host-native** — SoA floats or a
  compute shader, no byteswap, no LS, no `shufb` palette — i.e. the Task-B
  reimplementation, with all its Task-B difficulty. There is no free lunch in
  which a mechanical translation is both effortless *and* fast; the effort and
  the speedup are the same work (understand the intent, rewrite to host idioms),
  and for GPU-coupled per-frame geometry that work is large and risky.

---

## Bottom line

1. **EDGE is present but only as Zlib** (`1.2.3.0-PS3-SPU-EDGE`, zlib-1.2.3
   vintage). EDGE Geometry provenance for the hot kernels is **suggested by
   structure, not proven** — they are hookless dynamic job code with no EDGE
   symbol. Confirm before any RE; a usable reference impl is documented but not
   legally vendorable.
2. **Reimplementing the geometry frontier is a research project**, not an
   optimization: per-frame, RSX-coupled, no clean oracle, runtime-discovered
   hook. Multi-month, high-risk, and not a route to 60 fps. Audio + SPURS remain
   the wins.
3. **The IR→C++ transpile does not speed anything up.** rpcs3 already JITs that
   exact IR to native ARM64; a mechanical AOT of it hits the same backend and
   keeps the entire SPU-semantics tax (experiment: 8 native insns vs 1 for the
   same math). Its real value is human readability on the path to an
   intent-level host rewrite — which is exactly the hard Task-B work.

*Experiment artifacts: `scratchpad/spu_kernel.ll`, `scratchpad/native_skin.ll`,
`scratchpad/run_experiment.sh` (llc -O3, LLVM 21.1.2); kernel histograms:
`scratchpad/slice.py`; string sweeps: `scratchpad/edge_grep*.sh`.*
