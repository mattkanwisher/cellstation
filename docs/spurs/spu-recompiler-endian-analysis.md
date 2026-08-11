# How many endian conversions does RPCS3's ARM64 SPU recompiler emit?

Read from the source (rpcs3/rpcs3/Emu/Cell/SPULLVMRecompiler.cpp; the ARM64 SPU
path is the LLVM recompiler — there's no hand-written aarch64 ASMJIT SPU backend).
This CORRECTS the "~8x SPU-semantics tax / rev-per-op" figure cited earlier
(which came from a hand-written IR toy, not rpcs3's real output).

## The design
- **Registers are host-native (LE) NEON vectors** in `spu_thread::gpr` (v128).
  Register read/write and cross-block spills emit ZERO swaps. `A` (add) = 
  `match_vrs<u32[4]>` -> `a+b`, straight NEON.
- **Byteswaps only at the local-store boundary** (`make_load_ls` swaps after load,
  `make_store_ls` swaps before store; LS is stored big-endian). The swap is
  `byteswap = zshuffle(15..0)`, a full 16-byte reverse shufflevector -> ONE `tbl`
  (or rev64+ext) on ARM64, NOT a scalar rev per op.
- **Aggressive elision** via `match_expr(x, byteswap(...))`: load feeding a
  shufb/rotqby folds the reversal into the shuffle mask (0); splat is self-
  symmetric (0); load->store copy cancels the double-swap (0); constants
  pre-swapped at compile time (0).

## Exact counts (common sequences)
| Sequence | endian conversions |
|---|---|
| reg-to-reg ALU/FPU (a, fma, fnms, and, fm, ...) | **0** — 1 native NEON op each (4xFMA = fmla.4s) |
| shufb / rotqby permute (dominant scalar-access idiom) | **0** dedicated — folded into shuffle mask; 1 tbl total |
| lqd/lqx feeding arithmetic | **1** tbl; **0** if it feeds a shuffle/splat |
| stqd/stqx | **1** tbl; **0** if source was a load/shuffle |
| skinning inner step (5 lqd + 4 fma + 1 stqd) | ~6 tbl for memory; arithmetic 1:1 native |

## Consequences
1. The hot FMA-heavy geometry/skinning math runs at ~1:1 with native NEON — ZERO
   byteswaps. The real overhead is ~1 tbl per NON-elided LS access + the LS
   address mask, plus the pervasive shufb/rotqby marshaling (inherent to SPU
   being SIMD-only) that maps to tbl on ARM.
2. IR->C++ transpilation is even more clearly a non-win: the JIT already emits
   near-optimal NEON with no per-op swap; nothing mechanical to reclaim.
3. The intent-level geometry-rewrite win is NOT about eliminating byteswaps
   (there are ~none in the math) — it's about killing the shuffle-heavy data
   marshaling and LS pack/unpack round-trips (AoS->SoA, host-native layouts).
   That favors a host reimplementation or GPU-compute kernel over translation.

Caveat: verified from the IR-generation logic (where swaps are/aren't emitted),
not from disassembling the final JIT'd aarch64 — a compiled-output check would
confirm the exact tbl/rev lowering, but the emit sites are definitive.
