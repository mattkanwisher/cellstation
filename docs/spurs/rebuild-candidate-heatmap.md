# Pure-function rebuild candidates (in-fight heat map + purity drill-in)

Cross-referenced the in-fight profile (`fight-stock.folded`) with the fight
disassembly (all 4874 blocks). For each hot guest SPU kernel: cost, float-SIMD
density, "SPU-semantics tax" (shuffb/rotqby/byte-ops = the big-endian + local-
store overhead that an intent-native host rebuild eliminates, per the IR->C++
8->1 finding), and purity (does it do GETLLAR/PUTLLC reservations = shared/atomic
= NOT a clean rebuild, vs self-contained DMA-in/compute/DMA-out).

No guest symbols exist (SPU code is stripped) — kernels are hash + LS addr +
disasm, named by what their instructions do.

| Kernel | %CPU | float% | SPU-tax% | purity | rebuild |
|---|---|---|---|---|---|
| 0x0a538 | 2.99 | 19 | 21 | pure | **HIGH** — skinning math |
| 0x0cf24 | 3.07 | 10 | 12 | pure | med — marshaling/control |
| 0x05498 | 2.83 | 0  | 14 | pure | med — data reshaping (vertex decompress?) |
| 0x09438 | 2.64 | 17 | 31 | pure | **HIGH** |
| 0x0f54c | 2.10 | 15 | 22 | pure | **HIGH** |
| 0x0e5f0 | 1.73 | 20 | 23 | pure | **HIGH** |
| 0x004f8 | 1.45 | 1  | 23 | pure | (i3dl2 reverb — audio target) |
| 0x0a268 | 1.38 | 15 | 15 | pure (clean GET) | **HIGH** |
| 0x0f368 | 0.90 | 17 | 27 | pure | **HIGH** |
| 0x36bb4 | 0.86 | 28 | 21 | pure | **HIGH** |
| 0x0fbf0 | 1.05 | 1  | 10 | **atomic** GETLLAR | low (SPURS-contention, not rebuild) |
| 0x00a78 | 0.92 | 0  | 11 | **atomic** GETLLAR/PUTLLC | low |
| 0x0fb08 | 0.83 | 4  | 11 | **atomic** GETLLAR | low |

**7 pure, compute-dense HIGH kernels ≈ 11.7% of all CPU.** Big rebuild math win
(high SPU-tax → intent-native collapse), but these are the geometry/skinning
kernels: compute is self-contained (safe to intercept) yet OUTPUT feeds the RSX
vertex pipeline (pixel-visible correctness, per-frame). Each needs its I/O
contract traced (DSP-TRACE method that worked for audio) before substitution.

**Ladder:** audio (clean I/O, host libs — in progress) → trace 0x05498/0x0cf24
(low-float shuffle-heavy = likely vertex de/decompression, maybe less RSX-
coupled) → skinning cluster (biggest prize, needs RSX-output contract first).
The 3 atomic kernels are SPURS-scheduler territory, not rebuild.
