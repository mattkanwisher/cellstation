# Can DOA5's SPU skinning be offloaded to the GPU (Vulkan compute) or the Hexagon DSP?

A feasibility analysis for the AYN Thor (Snapdragon 8 Gen 2: Adreno 740 GPU +
Oryon-class CPU + Hexagon DSP), where rpcs3's RSX already runs on the Adreno via
Turnip/Vulkan and the Cell SPUs are emulated on the ARM cores.

**Scope:** analysis + prior-art only. No core code is modified here. Inputs are
the decompiled top-10 in-fight kernels (`kernel-decompilation/k01..k10`), the
HLE pipeline (`../SPU-HLE-PIPELINE.md`), the EDGE/IR-transpile study
(`edge-and-ir-transpile-analysis.md`), the HLE survey
(`hle-candidate-survey.md`), and the rebuild heat map
(`rebuild-candidate-heatmap.md`).

---

## TL;DR

- **The compute is trivial; the plumbing is everything.** The whole in-fight
  skinning/geometry cluster is ~1–2 GFLOP/s of real work. The Adreno 740 is a
  ~2.9 FP32-TFLOP part. It could do a frame's worth of skinning in **well under
  a millisecond of GPU-ALU time**. Nothing about this analysis is bottlenecked
  on GPU throughput — it is bottlenecked on **dispatch launch overhead, CPU↔GPU
  synchronization latency, and RSX-integration complexity**.

- **Per-job synchronous offload is a guaranteed loss.** SPU skinning jobs are
  KB-scale and there are 100s–1000s per frame. One Vulkan dispatch + one
  readback fence per job costs order **~200 µs each** (fence round-trip
  dominated). At even 200 jobs/frame that is **~40 ms/frame of pure overhead** —
  more than the entire frame budget, to replace ~single-digit ms of CPU work.
  Offload-and-copy-back **loses by 1–2 orders of magnitude**.

- **Only one configuration can win: batch + keep-it-on-the-GPU.** Coalesce a
  whole frame's skinning into a handful of dispatches in one command buffer, and
  **never copy the result back to the CPU** — leave the skinned vertices resident
  in a GPU buffer that Turnip's RSX vertex path reads in place. That removes both
  the per-job submit/fence cost and the CPU↔GPU serialization bubble. It also
  removes the determinism hazard of CPU readback. But it requires the SPU-HLE
  layer to integrate *tightly* with rpcs3's VK RSX backend and its guest-memory
  model — a multi-person-month structural project, not a drop-in HLE.

- **Determinism downgrades the oracle.** GPU IEEE FP32 ≠ SPU non-IEEE float
  (round-toward-zero, denormals-as-zero, no Inf/NaN, `frest`/`frsqest`
  estimates). Bit-exactness is impossible on the GPU path, so the recorded-I/O
  oracle degrades from a hard bit-diff (what makes the audio HLE safe) to a
  **tolerance/ULP comparison**. Acceptable *only* for graphics-only outputs that
  feed rasterization; **not** acceptable for any kernel whose results are read
  back into gameplay (physics/collision/hit-detection — i.e. k01).

- **Hexagon/HVX: reachable on paper, not worth it.** An unsigned-PD FastRPC path
  to HVX nominally exists, but it is fragile, undocumented for homebrew, carries
  its own dispatch/round-trip tax, and — critically — its output would still have
  to be copied back to feed the RSX (it is *not* on the GPU). It loses the one
  structural advantage (staying resident for the draw) that makes the Vulkan path
  potentially win. **Verdict: don't.**

- **Bottom line:** worth a **cheap PoC to kill-or-confirm the overhead question
  first** (a Turnip dispatch/round-trip microbenchmark — no kernel RE at all),
  and *only if that passes*, a single-kernel end-to-end-on-GPU prototype of **k06
  (position skinning)**. If the microbenchmark shows fence round-trips or
  required batching structure that can't fit DOA5's per-frame job stream, stop
  there — the numbers say it won't beat the CPU for KB-scale jobs unless the
  keep-on-GPU path is fully realized.

---

## 1. Per-kernel amenability

The question is per-kernel: does the work decompose into **independent per-lane
(per-vertex / per-bone) SIMD with no cross-lane dependency, no serial state, and
graphics-only output** — the shape a compute shader wants — or does it carry
control flow, serial accumulation, cross-element coupling, or gameplay-visible
output that a GPU cannot cleanly host?

| # | kernel | %CPU | GPU-amenable? | why |
|---|---|---|---|---|
| k04 | `spu_vertex_transform_project` | 2.64 | **Yes (clean)** | Per-vertex-independent T&L: matrix×vec, normal 3×3 + cross + rsqrt-normalize, perspective `1/w`, repack. No cross-vertex dep, no atomics. Caveats: a per-vertex **flag byte** (`0xb0`) selects paths → warp divergence (manageable via branch or SPIR-V specialization); **fan-out** to several output buffers → multiple stores (fine). This is the textbook compute-shader case. |
| k06 | `spu_skin_vertex_positions` | 1.73 | **Yes (clean — best PoC)** | Per-vertex matrix-palette position skinning with inline quat→matrix decode and weighted blend. Every vertex is independent. The **self-modifying LRU matrix cache** at LS `0x11790`/`0x11290` is *not* an obstacle — it is an artifact of the 256 KB local store; on the GPU you simply index the bone-palette buffer directly and **drop the cache entirely**. Output is positions → RSX. |
| k05 | `spu_skin_tangent_frame` | 2.10 | **Yes (clean)** | Per-vertex TBN skin + Newton-normalize + Gram-Schmidt orthonormalize + cross — all per-lane, no cross-vertex coupling. LRU cache dropped as in k06. The hot tail is a per-component fixed-point scale loop (`rotqby`/`shufb`-bound on SPU) that a compute shader does branch-free. Output is a tangent frame → RSX. |
| k08 | `spu_skin_transform_quantize_lod` | 1.38 | **Mostly (pack needs care)** | 4-wide SoA transform + normalize + **`cflts` fixed-point pack** + distance-LOD reject. Per-vertex transform/normalize is clean. The **LOD early-out** is per-*object* (evaluate on CPU or as a cull predicate — not per-vertex divergence). The `cflts` pack must reproduce the SPU's **round-toward-zero at scale `0xad`** to land in the exact RSX vertex format — the determinism-sensitive step (see §4). Net: amenable, with the quantizer as the fiddly part. |
| k02 | `spu_pose_build_matrix_to_quat_quantize` | 2.99 | **Partial — not worth it** | Two halves. The **math** (4×4 concat, branchless matrix→quat, Gram-Schmidt bone frames) is per-*bone* independent and GPU-clean — but it is **tiny** (hundreds of bones, not 100k vertices), so dispatch overhead dwarfs the work. The **pack loop** (dequant→`cflts`-requant→`gb`-mask→**bump-allocate** an append stream) is a **serial stream-compaction with a running allocator** — variable-length output, sequential dependency: GPU-hostile. Net: small + serial ⇒ poor target. |
| k01 | `spu_skeletal_transform_and_constraint_solve` | 3.07 | **No** | Index/adjacency traversal + inter-joint **distance tests** + reciprocal **positional correction**: this is a **constraint solve** — joint A's correction affects joint B through the adjacency structure = **cross-element data dependency**, the opposite of per-lane independence. Worse, it is physics that almost certainly **feeds gameplay** (bone collision, soft-body) → CPU readback → determinism desync hazard. Do not offload. |
| k03 | `spu_job_runtime_dispatch` | 2.83 | **No** | Opcode-stream interpreter + stackful fiber switch + free-list init. Pure control flow, 0% float. Not compute. |
| k07 | `spu_stream_cmd_dispatch_and_voice_alloc` | 1.45 | **No** | Tokenizer + `GETLLAR`/`PUTLLC` spinlocks + linked-list surgery + work-queue. Control/sync, 0% DSP float. (Audio-control, addressable by HLE, not GPU.) |
| k09 | `spurs_kernel_select_workload_cas` | 1.05 | **No** | Atomic RMW on the `CellSpurs` line. Scheduler sync. |
| k10 | `spurs_taskset_jobqueue_dequeue_cas` | 0.92 | **No** | Atomic RMW / job-queue dequeue. Scheduler sync. |

**GPU-amenable set:** k04, k05, k06, k08 — the per-vertex skinning/T&L kernels
whose output feeds the RSX. Together ≈ **7.9% of all in-fight CPU** (of the
~11.7% "pure HIGH rebuild" cluster). k02's math is amenable but too small; k01 is
structurally and semantically off-limits; k03/k07/k09/k10 are control/sync.

Two cross-cutting notes that *help* the GPU case:

- The **SPU-semantics tax is exactly what a compute shader deletes.** The
  IR→C++ study measured an **8:1** ratio of SPU-emulation instructions to useful
  math (`& 0x3FFFF` LS-wrap, `rev32`/big-endian byteswap, `shufb`→NEON `tbl`
  AoS↔SoA reshaping, register-file round-trips through the thread struct). A
  native compute shader over SoA float buffers has **none** of it — no local
  store, no byteswap, no permute palette. So per-vertex the GPU isn't just
  "another core," it does ~1/8 the instructions for the same result.
- The **LRU matrix cache** (k05/k06) and the **bump allocator** (k02) are SPU
  local-store coping mechanisms. On a GPU with a flat bone-palette buffer and
  fixed-stride output they simply vanish — one less thing to port. (The bump
  allocator only bites k02, which we're not targeting.)

---

## 2. The dispatch / sync / data-movement cost model — the real bottleneck

The useful work is negligible; the overhead is the whole story. Model each cost
term, then find the batching threshold.

### 2.1 The work is ~free on the GPU

Order-of-magnitude: two fighters, ~10^5 skinned vertices/frame across LODs,
~4 bone influences each, ~10^2 FLOPs/vertex ⇒ ~**10^7 FLOP/frame** ≈ **~1–2
GFLOP/s** at fight framerates. Against the Adreno 740's ~2.9 TFLOP/s FP32 that is
**<0.1% ALU utilization** — sub-100-µs of actual shader execution. The GPU has
~100–1000× compute headroom. **Every conclusion below is about overhead, never
throughput.**

The CPU side it replaces: the amenable cluster is ~7.9% of *total* profile CPU
(all threads). Expressed as wall-clock that is **single-digit milliseconds of
aggregated SPU-worker time per frame** (exact figure depends on core count and
fps; the point is it is a few ms, spread over the 5 MAIN workers — not tens of
ms). So the GPU path has to beat "a few ms of CPU," and it has to do so
*including* all overhead.

### 2.2 The overhead terms (Adreno / Turnip, mobile)

Numbers below are drawn from published Vulkan/WebGPU dispatch measurements
(desktop wgpu/Vulkan ~25–36 µs per dispatch, submit-dominated ~13 µs) adjusted
for mobile, plus mobile-GPU latency characteristics. Treat as
**order-of-magnitude**, to be replaced by the PoC microbenchmark (§8).

| term | rough cost | notes |
|---|---|---|
| CPU record per dispatch (bind pipeline + descriptor + `vkCmdDispatch`) | **~1–3 µs** | cheap; scales with dispatch count but stays in a recorded command buffer |
| `vkQueueSubmit` (per submit, **not** per dispatch) | **~10–50 µs** | mobile drivers heavier than desktop; **amortizable** — one submit can carry many dispatches |
| **Fence round-trip** (submit → GPU wake+schedule+execute → CPU observes signal) | **~100–500 µs** | *the killer.* Mobile tiled GPUs optimize throughput, not latency; the wake/schedule/signal path is long even for trivial work. Blocks the CPU thread and serializes CPU↔GPU. |
| Data movement (job I/O CPU↔GPU) | **~0.5–1 µs per 16 KB** | on **unified memory**, essentially the memcpy time at ~20 GB/s LPDDR5X — or **zero** if the buffer is `HOST_VISIBLE | DEVICE_LOCAL` and shared in place. Never the bottleneck. |
| GPU-side pipeline barrier (compute→vertex, intra-GPU) | **~single µs** | cheap; this is what replaces the fence in the keep-on-GPU path |

**Unified memory, quantified.** On the Thor the CPU and Adreno share LPDDR5X.
A discrete GPU would stage each 16 KB job over PCIe; here there is *no bus copy*.
If the skinning output buffer is allocated `HOST_VISIBLE | DEVICE_LOCAL` (Adreno
advertises this combo), the compute shader reads the exact physical bytes the SPU
wrote and the RSX reads the exact bytes the compute wrote — **zero-copy, staging
buffers eliminated**. But UMA's win is the *copy*, which was already ~1 µs and
never the bottleneck. **UMA does not touch the dispatch-launch or fence-round-trip
costs — the terms that actually dominate.** So "unified memory helps" is true but
it helps the cheap term, not the expensive one. Its *real* value is structural:
it is what makes the keep-on-GPU path (§3) physically possible.

### 2.3 Three regimes and the batching threshold

Let `N` = skinning jobs/frame (hundreds to low thousands for DOA5).

**Regime A — one dispatch + one readback fence per job (naive offload).**
Cost ≈ `N × (record + submit + fence)` ≈ `N × ~200 µs`.
At `N = 200`: **~40 ms/frame** of pure overhead. **Catastrophic loss** —
>10× the CPU work, over the whole frame budget. This is the "offload-and-copy-back
per job" strawman and it is dead on arrival.

**Regime B — batch all jobs into one command buffer, one submit, one readback
fence.** Cost ≈ `N × record + 1 × (submit + fence)` ≈ `N × ~2 µs + ~300 µs`.
At `N = 200`: **~0.7 ms/frame**. Now *under* the CPU cost — a win on paper. But
this requires **deferring the entire frame's skinning** so the jobs can be fired
together, and it still ends in a **CPU readback fence**: the skinned vertices come
back to main memory (reintroducing the determinism hazard, §4) and the CPU stalls
on the fence (a serialization bubble with no CPU↔GPU overlap). Marginal, fragile,
and only viable if nothing needs a skinning result mid-batch.

**Regime C — batch + keep-it-on-the-GPU (no readback).**
Cost ≈ `N × record` ≈ `N × ~2 µs` ≈ **~0.4 ms/frame**, **zero CPU stall**, no
determinism-sensitive readback. The compute output stays in a GPU buffer; the RSX
vertex path consumes it after a cheap intra-GPU barrier. **This is the only regime
that clearly and safely wins.** It is also the hardest to build (§3).

**Threshold statement.** To beat the CPU you must amortize the submit+fence to
**≈ single-digit dispatches per frame** (ideally one), which is only possible if
you (a) **coalesce** many SPU jobs into few dispatches — concatenate per-frame
vertex batches into one big buffer and launch one workgroup-per-batch dispatch —
and (b) **remove the readback** so there is no per-frame fence on the critical
path. Coalescing requires intercepting and reordering the SPU job stream so the
frame's skinning is collected before firing; removing the readback requires the
keep-on-GPU integration in §3. **Batching is not an optimization here — it is the
precondition for the idea to be net-positive at all.**

---

## 3. The structural opportunity: keep it on the GPU, feed the RSX directly

This is the make-or-break, and it follows directly from §2: the pipeline is
already **SPU skinning → vertex buffer → RSX draw**, and the RSX is *already* on
the Adreno via Turnip. So the skinned output could, in principle, never leave the
GPU:

```
  today:   SPU skin (ARM) → PS3 main memory → RSX/Turnip reads vertex buffer → draw
  target:  compute skin (Adreno) → GPU buffer ──(intra-GPU barrier)──▶ RSX/Turnip vertex fetch → draw
                                        └ same bytes also visible at guest EA via UMA (if needed)
```

If realized, this deletes the two costs that kill regimes A/B: no per-frame CPU
readback fence (only a GPU-side compute→vertex barrier, ~µs), and no copy (UMA).
The CPU never sees the skinned data, so there is no determinism-sensitive
round-trip either. **This — not raw compute — is the entire reason the idea could
be a net win.** Offload-and-copy-back loses; offload-and-stay-resident wins.

### How tightly would it have to integrate?

Very. The hard parts are all in the seam between the SPU-HLE layer and rpcs3's VK
RSX backend and guest-memory model:

1. **No clean hook.** Per the EDGE analysis, the skinning kernels are **dynamic
   job code** DMA'd into local store by the game's `Rel…6e2594` executor — they
   have **no image hash to intercept at `sys_spu_image::deploy`** (the clean
   mechanism the audio HLE uses). Interception must hook **job-code upload** —
   recognize the descriptor/entry the executor is about to `bisl` into — a
   materially weaker, more fragile hook.

2. **Descriptor-format reverse engineering.** A compute shader must parse the
   exact bone-palette layout, influence-stream strides (144 B / 160 B records),
   quantization scales (the `0xad` `cflts` scale, the `r35`/`r34` dequant
   scale/bias), and vertex formats — the SDK-licensed EDGE details that are *not*
   in any source we can vendor. This is per-kernel RE, gated by the recorded-I/O
   oracle.

3. **Guest-EA ⇄ GPU-buffer aliasing.** The RSX draw references the skinned buffer
   by **guest effective address** (the game patches a draw to point at it). To
   keep the output on the GPU, that EA range must be **backed by (or coherently
   aliased to) the Vulkan buffer the compute shader wrote**, so RSX vertex fetch
   picks it up with no copy. rpcs3 maps guest RAM as a host allocation, not
   necessarily a bindable Vulkan buffer; making a skinned-output EA range be a
   `HOST_VISIBLE | DEVICE_LOCAL` VkBuffer that both the compute pass writes and
   the RSX binds is **significant plumbing that fights rpcs3's memory model.**
   (Local memory note: MemoryManager reservations are load-bearing on Android —
   *don't* perturb them — so this aliasing has to be additive, not a remap.)

4. **RSX synchronization.** The compute→draw dependency must be expressed as a VK
   pipeline barrier inside Turnip's frame, and the RSX's existing cache-invalidate
   / vertex-buffer-dirty tracking must not re-upload the guest EA from CPU memory
   on top of the GPU-written data. This couples the SPU-HLE scheduler to the RSX
   frame boundary.

**Assessment.** Steps 1–2 are the per-kernel RE cost (weeks each). Steps 3–4 are a
**one-time, multi-person-month RSX-integration cost** shared across all kernels —
and they are the genuinely novel, genuinely risky engineering. Without 3–4 you
are stuck in regime B (readback), which is marginal and determinism-exposed. **The
project's value is entirely gated on whether the keep-on-GPU integration is
achievable in rpcs3's VK backend.** That is the first thing a serious effort must
de-risk (see the PoC, §8) — and it is exactly the wall the EDGE analysis already
flagged (GPU-coupled, per-frame, runtime-discovered hook, no clean oracle), now
made concrete.

---

## 4. Correctness / determinism and the oracle

### GPU FP32 ≠ SPU FP32

The SPU single-precision FPU is **deliberately non-IEEE**: round-toward-zero only,
**denormals flushed to zero**, **no Inf/NaN** (it saturates to the max normal),
and the reciprocal/rsqrt primitives (`frest`, `frsqest`) are **~12-bit hardware
estimates** — the guest code then does explicit Newton-Raphson refinement on top
(seen in every normalize idiom across k01/k02/k04/k05/k06/k08). rpcs3 models this
with its **`xfloat` accuracy** setting (Accurate / Approximate / Relaxed); Accurate
faithfully reproduces the SPU rounding, and even it disables denormals for SPU
threads.

A Vulkan compute shader runs **IEEE-754 round-to-nearest FP32 with real Inf/NaN**.
Consequently:

- **Bit-exactness is impossible.** Even a line-for-line port of the guest
  arithmetic diverges in the low bits (different rounding, different `frsqest`
  seed, different denormal handling). You cannot bit-match the SPU on the GPU.
- **The refined normalizes are *close* anyway.** Because the guest already Newton-
  refines its estimates, the final normalized vectors land within a few ULP of an
  IEEE computation. For **geometry that feeds rasterization**, the error is
  **sub-pixel and perceptually invisible** — perceptual-close is the right bar.
- **The quantized pack is the sharp edge.** k08's `cflts` at scale `0xad` and
  k05's output scales produce the **fixed-point integers the RSX vertex format
  consumes**. A rounding difference here can flip a low bit of a packed coordinate
  — still sub-pixel, but the shader must reproduce the SPU's **round-toward-zero**
  (GLSL/SPIR-V default is round-to-nearest; use truncation `int(x)` semantics
  deliberately) so the packed field width/scale stay valid.

### The determinism firewall

The critical distinction is **where the output goes**:

- **Graphics-only output (k04, k05, k06, k08 → RSX):** perceptual-close is fine,
  *provided the data never returns to the CPU.* In the keep-on-GPU path (§3) it
  doesn't, so FP divergence only ever manifests as sub-pixel geometry — safe.
- **Gameplay-visible output (k01 constraint solve, and any skinned position the
  game reads back for collision/hit-detection/camera):** FP32-vs-xfloat
  divergence **accumulates and can desync gameplay** (and would break replay/
  netcode determinism if present). This is a hard **no-offload** boundary. It is
  the main reason k01 is excluded independently of its cross-lane structure, and
  the reason the keep-on-GPU (no-readback) path is not just a performance choice
  but a **correctness requirement**: the moment a skinned result is copied back to
  the CPU, it can leak into simulation.

### What the oracle can and can't do here

The recorded-I/O oracle (`SPU-HLE-PIPELINE.md` §6) is what makes the audio HLE
safe: capture real input→output pairs, replay through the replacement, **bit-diff**.
On the GPU geometry path it **degrades**:

- The PUT'd output buffer is still observable, so the oracle *mechanism* applies —
  but the comparison must become **tolerance-based** (per-component ULP/epsilon,
  or a packed-field-with-±1-LSB allowance), not bit-exact. You lose the hard gate.
- The correctness surface is **larger and pixel-visible**: "correct" now means
  "byte-plausible RSX vertex buffer *and* consistent with the GCM draw stride/
  format/offset that consumes it," not "silence in the background." A tolerance
  pass can miss a format/stride mismatch that a bit-diff would have caught.
- Practical oracle for a PoC: record the SPU output for a fixed pose, run the
  compute shader on the same input, and check **max-abs-error and the rendered
  frame** against the LLE reference — accept if geometry is sub-pixel-identical.
  This is weaker than the audio oracle but sufficient for graphics-only kernels.

**Verdict:** perceptual-close is acceptable for the four graphics-only kernels
**iff** the keep-on-GPU path guarantees no CPU readback; bit-exactness is neither
achievable nor required for them; and the oracle downgrades from a hard gate to a
tolerance check, which is the price of the FP model mismatch.

---

## 5. Translation approach: hand-written GLSL/SPIR-V per hash, not auto-transpile

Two candidate routes, mirroring the audio HLE decision:

- **Hand-written GLSL/SPIR-V compute keyed by SPU hash (recommended).** Same shape
  as the per-hash audio HLE: identify the kernel by content hash (here, by its
  uploaded-job entry signature, since it has no image hash), understand its I/O
  contract from the disasm + oracle trace, hand-write a compute shader that reads
  the same descriptors and produces the same vertex output, verify against the
  (tolerance) oracle. This is the realistic path. It reuses the intent-level
  understanding already captured in the k01–k10 decompilations.

- **Automatic SPU→SPIR-V transpile (not viable).** The EDGE/IR study already
  settled the mechanical-translation question for the CPU target and it transfers:
  rpcs3's SPU IR is **not free-floating math** — it is GEPs into `spu_thread::gpr[128]`,
  `%ls`-pointer loads with the `& 0x3FFFF` wrap, big-endian byteswaps, and calls
  into the MFC/DMA/reservation runtime. Lowering *that* to SPIR-V would carry the
  **entire SPU-semantics tax onto the GPU** (the 8:1 tax becomes `tbl`-soup
  compute shaders), i.e. you'd emulate the SPU *on the GPU* rather than do
  skinning on the GPU — the worst of both worlds, and it would need the SPU
  runtime linked into a shader, which is impossible. Auto-transpile defeats the
  only reason to use the GPU (deleting the semantics tax). **Intent-level hand
  rewrite is the only approach that captures the win.**

### Effort estimate (per kernel, hand-written)

| phase | cost | notes |
|---|---|---|
| Trace the I/O contract (descriptors, strides, scales) via oracle record mode | ~1–2 weeks | reuses k0x understanding; the descriptor formats are the unknown |
| Write + debug the compute shader (GLSL→SPIR-V), match the pack/quantize | ~1–2 weeks | per-vertex math is easy; the `cflts`/round-toward-zero pack and flag-path divergence are the fiddly bits |
| Verify vs tolerance oracle + rendered-frame check | ~1 week | weaker gate than audio; needs a reference-frame diff |
| **per-kernel subtotal** | **~3–5 person-weeks** | for the *offload-and-readback* (regime B) version |

Plus the **shared one-time RSX-integration cost (regime C, §3): multiple
person-months** — the job-upload hook, guest-EA⇄VkBuffer aliasing, and RSX
compute→vertex synchronization. **This shared cost, not the per-kernel cost, is
the real budget line**, and it buys nothing until at least one kernel rides it end
to end.

---

## 6. Hexagon / HVX alternative — honest verdict: don't

The Hexagon DSP is architecturally the closest thing on the SoC to an SPE: HVX is
a wide (1024-bit) SIMD vector unit, and SPU skinning is wide SIMD. On paper it's a
natural fit. In practice, on a **production Snapdragon 8 Gen 2** in an
**unprivileged Android app**, it is the wrong tool:

- **Accessibility is gated by code signing.** DSP libraries are normally
  **signed by Qualcomm**; "a regular Android application has no permissions to
  execute its own code on the DSP." The escape hatch is the **Unsigned PD**
  (user protection domain) via FastRPC, which runs low-rights signature-free
  shared objects — but it is "limited in its access to underlying DSP drivers and
  thread priorities," "designed to support only general computing applications,"
  and untrusted apps face checks that can block opening the FastRPC device node
  or spawning even a signed PD without a privileged helper. (Historically only
  specific SoCs — e.g. SD 855/865 — were called out as freely allowing
  signature-free cDSP code; on newer parts the unsigned-PD path exists but is
  undocumented-for-homebrew and OEM/firmware-dependent. On the AYN Thor this is
  unverified and fragile.) Building against the **Hexagon SDK** (toolchain,
  `skel`/`stub` marshaling, calibration) is a substantial standalone effort
  before a single vector op runs.

- **Same dispatch/round-trip tax, worse ecosystem.** FastRPC is an RPC across the
  APPS↔cDSP boundary with its own marshaling and latency; you pay a per-call
  overhead comparable in spirit to the Vulkan dispatch tax, and you must batch the
  same way. No advantage there.

- **It loses the one thing that makes the GPU path win.** The whole thesis (§3)
  is *keep the skinned vertices resident where the consumer already lives* — and
  the consumer (RSX) is on the **GPU**. HVX output would have to be **copied back**
  to feed the RSX/Turnip vertex path. That is regime B (offload-and-copy-back)
  permanently — the losing regime — with no path to the keep-on-GPU win. Even if
  HVX were trivially accessible, it would be architecturally behind the GPU for
  *this* workload because the data's destination is the GPU.

- **Integration difficulty vs Vulkan compute.** Vulkan/Turnip is already linked,
  already running the RSX, already sharing memory with the target consumer.
  Hexagon adds a second heterogeneous runtime (FastRPC + Hexagon SDK + signing
  dance) for output that then has to cross back to the GPU anyway.

**Verdict: not reachable in a way that helps.** Nominally possible via unsigned-PD
FastRPC, practically fragile/undocumented on a production 8 Gen 2, and
**architecturally pointed the wrong way** — its results live on the DSP, but the
consumer lives on the GPU. Spend the effort on Vulkan compute, where the output is
already next to the RSX.

---

## 7. Prior art

**No emulator ships GPU-compute offload of a guest CPU/DSP/vector coprocessor's
math, and rpcs3 in particular does not.** What exists:

- **rpcs3's SPU emulation is CPU-side LLVM/ASMJIT recompilation** — SPU 128-bit
  vector ops → host SSE/AVX (or NEON on ARM). The 2024–2026 "Cell breakthrough"
  (lead dev Elad) that made headlines was **better native CPU codegen** from SPU
  usage patterns — *not* a GPU offload. rpcs3 *does* use the GPU for host-side
  helpers the real RSX wasn't doing (texture decode, image/format conversion), but
  never for SPU compute. So the specific idea here is **unattempted in rpcs3.**
  ([DeepWiki: GPU emulation](https://deepwiki.com/RPCS3/rpcs3/2.3-gpu-emulation),
  [Tom's Hardware: Cell breakthrough](https://www.tomshardware.com/video-games/playstation/rpcs3-ps3-emulator-gets-cell-cpu-breakthrough-that-improves-performance-in-all-games))

- **PCSX2 GS on compute** is the closest real precedent, but note what it is: the
  PS2's **GS is a graphics rasterizer**, and its hardware/compute renderer paths
  reimplement *graphics* on the host GPU. That is emulating a GPU on a GPU — not
  offloading a *general vector CPU's* compute to the GPU. Same for Dolphin's
  ubershaders / GX-to-GPU work: graphics on graphics.

- **The academic literature on Cell/SPU emulation** (e.g. the CBEA-difficulty
  analyses) documents *why* SPU emulation is hard — DMA-driven local store, dual
  ISA, non-IEEE float — but does **not** describe GPU-compute offload of SPU
  kernels as a solution. The recurring conclusion is CPU-side recompilation.
  ([ResearchGate: CBEA emulation-difficulty analysis](https://www.researchgate.net/publication/375600995_An_Analysis_of_the_CELL_Broadband_Engine_Architecture_and_its_Implications_on_the_Difficulty_of_Emulating_the_PlayStation_3_Console))

- **Adjacent, transferable evidence — mobile GPU dispatch overhead is the known
  wall.** The on-device-LLM-inference literature independently confirms the §2
  thesis: on mobile Adreno, **eliminating dispatch overhead is worth ~35% of
  throughput** (SD 8 Gen 4 LLM study), per-dispatch overhead is **submit-dominated**
  (~13 of ~32 µs on wgpu/Vulkan), and fine-grained shared virtual memory cut
  OpenCL sync overhead **from 162 µs to 7 µs** on an Adreno phone — i.e. the same
  conclusion (batch, keep-resident, avoid round-trips) that this analysis reaches
  for skinning.
  ([Vulkan compute kernels for Android LLM inference](https://mvpfactory.io/blog/custom-vulkan-compute-kernels-for-on-device-llm-inference-on-android-bypassing/),
  [WebGPU dispatch-overhead study](https://arxiv.org/pdf/2604.02344),
  [Adreno best practices](https://docs.qualcomm.com/nav/home/mobile_best_practices.html))

- **Hexagon/HVX acceleration research** (the user's referenced line): Qualcomm's
  own Hexagon DSP developer material and third-party analyses
  ([Hexagon DSP CPU-offload](https://mdeore.medium.com/hexagon-dsp-cpu-offload-4fb8e4077fe8),
  [chipsandcheese: Hexagon DSP/NPU](https://chipsandcheese.com/p/qualcomms-hexagon-dsp-and-now-npu))
  describe HVX offload for signal/ML workloads via FastRPC, and the security
  research on the signing model
  ([Check Point: Pwn2Own Qualcomm DSP](https://research.checkpoint.com/2021/pwn2own-qualcomm-dsp/))
  is the source for the signed-PD / unsigned-PD accessibility constraints in §6.
  None of it targets game-console vector-coprocessor emulation.

**Summary:** the idea is essentially **novel** for SPU emulation. The closest
precedents (PCSX2/Dolphin) are graphics-on-graphics, not CPU-vector-on-GPU. The
transferable prior art is the mobile-GPU-inference community, and it says the
overhead model in §2 is right: **batch, keep resident, avoid CPU↔GPU round-trips,
or you lose.**

---

## 8. Bottom line and the minimal PoC

### Is it worth a proof-of-concept?

**Worth a *cheap, staged* PoC — yes. Worth committing to the full build up front —
no.** The reasoning:

- The upside is real and large (~7.9% of in-fight CPU is genuinely GPU-shaped, and
  the GPU has 100–1000× compute headroom), and it is the one attack on the ~14%
  geometry frontier that plays to the hardware the port already has (Turnip/Adreno,
  unified memory, RSX already on-GPU).
- But it is a **loss in every regime except keep-it-on-the-GPU**, and that regime
  costs multiple person-months of RSX-backend integration with pixel-visible
  failure modes, a downgraded (tolerance-only) oracle, a fragile job-upload hook,
  and SDK-licensed descriptor formats to reverse. Per the EDGE analysis this is a
  research project, not a drop-in optimization.

So the discipline is: **spend a few days to kill the overhead question before
spending a month on integration.**

### Minimal PoC — two cheap steps, gated

**Step 0 (days, no kernel RE): a Turnip dispatch/round-trip microbenchmark on the
actual Thor.** This answers the make-or-break §2 question with almost no code:

- Launch trivial compute dispatches through Vulkan/Turnip on the Adreno 740 and
  measure, on-device: (a) CPU record cost per `vkCmdDispatch`; (b) `vkQueueSubmit`
  cost; (c) **fence round-trip latency** for 1, 10, 100, 1000 dispatches batched
  into one command buffer; (d) `HOST_VISIBLE | DEVICE_LOCAL` buffer write→GPU
  read→CPU read coherency cost for a 16 KB "job."
- **Decision gate:** compute the regime-B and regime-C per-frame costs from the
  measured numbers for DOA5's job count. If even the idealized regime C can't get
  under ~1 ms/frame, or if the fence round-trip is so large that any required CPU
  readback blows the budget, **stop — the numbers killed it** and the doc's
  pessimistic case is confirmed with real hardware figures. If regime C is
  comfortably sub-millisecond (expected), proceed.

**Step 1 (weeks, one kernel): end-to-end-on-GPU prototype of k06
(`spu_skin_vertex_positions`).** Chosen because it is the cleanest fully
independent per-vertex kernel — matrix-palette position skinning with inline
quat→matrix decode, no cross-lane dependency, no atomics, LRU cache droppable,
and **graphics-only output** (positions → RSX, no gameplay readback ⇒ the
determinism firewall holds).

- Build it in the **keep-on-GPU** configuration from the start (regime C) — the
  point of the PoC is to de-risk the **RSX-integration seam** (§3 steps 3–4: hook
  job upload, alias the skinned-output EA to a VkBuffer, express the compute→
  vertex barrier in Turnip), because that seam — not the shader — is where the
  project lives or dies. A regime-B (readback) version is a fallback only for
  measuring correctness, not the target.
- **Measure:** (1) rendered-frame diff vs the LLE reference (sub-pixel? any
  glitch/tearing from the aliasing or barrier?); (2) max-abs per-vertex error vs
  the recorded SPU output (tolerance oracle); (3) **frame-time delta** — does
  moving k06's ~1.7% off the SPU workers actually show up as headroom, or does the
  integration overhead eat it?; (4) does the job-upload hook reliably catch every
  frame's k06 without false positives.

**What "success" looks like:** k06 renders identically (sub-pixel), the SPU
workers measurably shed k06's cost, and the RSX-integration overhead is
negligible — proving the seam works. *Then* k04/k05/k08 ride the same
integration at ~3–5 weeks each. **What "stop" looks like:** the aliasing fights
rpcs3's memory model, the barrier causes RSX re-upload or stalls, or the frame-
time delta is a wash — in which case the ~14% geometry frontier stays a research
project and the effort belongs on the confirmed wins (audio HLE, SPURS scheduler)
that the survey already ranks above it.

**Recommendation:** run Step 0. It is a few days, it produces hard Thor numbers,
and it converts this analysis from estimates to a decision. Do not start Step 1
until Step 0's gate passes.
