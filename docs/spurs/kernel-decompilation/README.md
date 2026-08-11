# DOA5U hot SPU kernels — named symbol table (top 10 in-fight)

SPU guest code is stripped (no symbols). These names + reconstructions were
recovered by decompiling the fight-time disassembly (5 parallel agents, 2
kernels each; full C++ reconstructions in k01-k02.md … k09-k10.md). Each is a
576-instruction window into a larger job, so structure is inferred from idioms.

| # | LS/hash | %CPU | name | class | provenance |
|---|---|---|---|---|---|
| k01 | 0x0cf24 | 3.07 | `spu_skeletal_transform_and_constraint_solve` | physics/collision constraint solve (transform+normalize+distance-nudge) | bespoke engine |
| k02 | 0x0a538 | 2.99 | `spu_pose_build_matrix_to_quat_quantize` | anim math: matrix concat, matrix→quat, dequant/requant pack, Gram-Schmidt | bespoke engine |
| k03 | 0x05498 | 2.83 | `spu_job_runtime_dispatch` | SPU job runtime/marshaler + fiber context switch (NOT decompression) | bespoke SPURS-task |
| k04 | 0x09438 | 2.64 | `spu_vertex_transform_project` | per-vertex T&L: transform + normalize + perspective project | stock/near-stock EDGE |
| k05 | 0x0f54c | 2.10 | `spu_skin_tangent_frame` | TBN-basis skinning + Newton renormalize + Gram-Schmidt | EDGE skeleton + bespoke ext |
| k06 | 0x0e5f0 | 1.73 | `spu_skin_vertex_positions` | matrix-palette position skinning w/ inline quat→matrix decode | EDGE skeleton + bespoke ext |
| k07 | 0x004f8 | 1.45 | `spu_stream_cmd_dispatch_and_voice_alloc` | audio-engine command dispatcher + voice/buffer free-list (GETLLAR/PUTLLC) — **NOT the reverb** | bespoke |
| k08 | 0x0a268 | 1.38 | `spu_skin_transform_quantize_lod` | SoA skinning + normalize + fixed-point pack + distance-LOD cull | bespoke, EDGE-inspired |
| k09 | 0x0fbf0 | 1.05 | `spurs_kernel_select_workload_cas` | SPURS workload select/claim atomic RMW | Sony SPURS |
| k10 | 0x00a78 | 0.92 | `spurs_taskset_jobqueue_dequeue_cas` | SPURS taskset job-queue dequeue + priority scan | Sony SPURS |

## Categorized by attack surface (top-10 ≈ 20% of all in-fight CPU)

- **Geometry/skinning/animation compute ≈ 13.9%** (k01,k02,k04,k05,k06,k08): the
  "rebuild for speed" frontier. MIXED provenance — only k04 is stock-ish EDGE;
  k01/k02/k05/k06/k08 are bespoke Team Ninja engine code (inline quat decode,
  fused TBN orthonormalize, LOD reject, `heqi` assert traps). So NO free
  reference impl for most of it, and it's RSX-coupled per the EDGE analysis —
  hard, per-kernel intent-level rewrites, each needing an I/O-contract trace.
- **Job-runtime / audio-control dispatch ≈ 4.3%** (k03 job dispatcher, k07 audio
  cmd dispatcher): NOT compute. Marshaling/fiber-switch/free-list + lock-line
  spinlocks. Two of my earlier labels were WRONG here (k03≠decompression,
  k07≠reverb) — both are control/dispatch, addressable by SPURS/runtime HLE, not
  a math rebuild.
- **SPURS scheduler sync ≈ 2.0%** (k09,k10): pure GETLLAR/PUTLLC on the CellSpurs
  line → eliminated by the SPURS-kernel HLE (patch 0020).

## Strategic takeaway
The SPURS-kernel HLE plausibly reaches k03+k07+k09+k10 ≈ **6.3%** (dispatch +
sync), more than the sync alone — strengthening that project. The ~14% geometry
compute is the biggest but hardest frontier (bespoke, RSX-coupled). Audio DSP
(separate from these, ~one core incl. ATRAC) remains the cleanest win.
