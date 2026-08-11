# A generalizable SPU-function HLE pipeline

The idea, in one line: **take an SPU function by its content hash, understand it
from rpcs3's own disassembly + LLVM IR, hand-write (or generate) a C++ replacement,
intercept the guest at that function's entry, and run the C++ instead — with a
recorded input/output oracle proving the replacement is bit-exact before it is
ever trusted.**

This turns the one-off SPURS-kernel HLE (patch 0020) and the audio-stub experiment
into a repeatable machine. It is the concrete form of "AI-generated per-game
optimization, Tier 2" from `docs/RESEARCH-ai-optimization.md`.

## Why SPU functions are the right unit

- **They are addressable.** Every SPU image is SHA1-hashed at load
  (`sys_spu_image::deploy`, `PPUModule.cpp` for embedded images). A function is
  identified by `SPU-<sha1>` — stable across runs, keyable in a registry, exactly
  how the patch engine already targets code.
- **They are near-pure.** An SPU job is DMA-in → compute → DMA-out. Given the same
  local-store inputs it produces the same outputs. That makes a **recorded I/O pair
  a real test oracle** — no trust in the C++ (or the AI that wrote it) required.
- **They are where the cycles are.** DOA5 profile: 58% of CPU is guest SPU code.
  The removable/replaceable share (SPURS scheduler, ATRAC decode, mstream DSP) is
  a large chunk of it.

## The pipeline

```
  hash ─▶ extract ─▶ understand ─▶ generate C++ ─▶ intercept ─▶ verify ─▶ ship
   │        │            │             │              │            │
  SHA1   spu.log +    IR/asm read   host impl    stop-code    replay recorded
  from   spu-ir.log   (semantics)   of the fn    at entry     I/O, bit-diff
  deploy  (SPU Debug)                             (registry)   outputs
```

### 1. Identify (hash)
Boot the title; every loaded SPU image logs `SPU executable hash: SPU-<sha1>`
with its SPUNAME. Pick targets by name/profile. Known DOA5 targets:

| SPUNAME | SHA1 | role |
|---|---|---|
| SPURS kernel2 | `756df5d819d66f930a0c4e18c72a0e9972fa45fa` | scheduler (patch 0020 WIP) |
| `msngSPURS_ATRAC` | `702a721b683f370f79d5653baded1b015118d2ea` | ATRAC3+ decode |
| `mstream_dsp_i3dl2` | `6783990ef3164dee43c67f62758719c58ccccc9e` | reverb |
| `mstream_dsp_para_eq` | `e433af6d732c289fddceca260581a622e2a356d1` | EQ |
| `mstream_dsp_filter` | `2379c584746bcc87e8d87e1fdcd112600d090dc3` | filter |
| `mstream_dsp_meter` | `b5522754fbebb4db82061beea60da968ff46da64` | level meter |
| `PS3SPUIoZlib` | `941db4cddf8046a0a1e6cb8527313eacd0dc382e` | inflate (load-time only) |

### 2. Extract (rpcs3 IS the disassembler)
`SPU Debug: true` in config.yml makes rpcs3 dump, per SPU function:
- `spu.log` — `SPUDisAsm` disassembly (`> LSADDR: bytes  mnemonic ops`).
- `spu-ir.log` — the LLVM IR the recompiler builds (huge — starts with the
  interpreter template; the per-function IR follows).
- `spu_progs/*.elf` — the raw job ELF (position-independent, `e_entry` at 0x90 for
  the mstream `.pic` jobs).

Helper: `scratchpad` disassembler `spudis.py` exists but rpcs3's own output is
authoritative — prefer grepping `spu.log` by a distinctive byte word to locate a
job's LS-resident copy, then slice the address range.

**Turn SPU Debug OFF before any perf measurement — it forces the interpreter and
is a large slowdown.**

### 3. Understand
Read the IR/asm for the I/O contract, not the math:
- entry register convention (`r3` = job/descriptor pointer for the mstream jobs),
- the `wrch MFC_LSA/EAH/EAL/Size/TagID` + `wrch MFC_Cmd, GET/PUT` DMA setup,
- the completion signal (see the gotcha below).

### 4. Generate the C++ replacement
A host function `bool hle(spu_thread&)` that reads the same inputs (registers +
LS, issuing DMA through the SPU thread's MFC as needed), computes the same
outputs, writes them back, and returns control. For codecs the body is an
existing host library (ffmpeg ATRAC3+, zlib). For DSP effects it is a small
kernel (biquad EQ, comb/allpass reverb) — or a **passthrough** (copy input→output)
when the goal is only to remove the compute for measurement.

### 5. Intercept (generalize patch 0020)
Patch 0020 already proved the mechanism for the SPURS kernel: at
`sys_spu_image::deploy`, when the image hash is registered, write a reserved SPU
**stop opcode** (0x3F80–0x3F85 today) into local storage at the function entry;
`spu_thread::stop_and_signal` dispatches it to the C++ handler. Generalize to a
registry:

```cpp
// hash -> { entry_ls_addr, handler }
struct spu_hle { u32 entry; bool(*fn)(spu_thread&); };
static const std::unordered_map<std::string, spu_hle> g_spu_hle = { ... };
```

so any hashed function can be diverted, gated behind a config flag + per-hash
enable (same shape as the audio-stub `patch_config.yml`).

### 6. Verify (the recorded-I/O oracle — the part that matters)
Add a **record mode**: for a target hash, capture at the real function's entry the
input register state + the LS/DMA inputs it reads, and at exit the outputs it
writes. Store input→output pairs. To trust a C++ HLE, replay the recorded inputs
through it and bit-diff the outputs against the recording. Only enable an HLE that
passes. This is what makes an AI-written replacement safe: the AI is replaceable,
the oracle is not.

## The gotcha this session found (why a naive stub deadlocks)

The mstream DSP jobs **self-manage their I/O and completion** — each does its own
`MFC GET` to read the audio buffer, processes it, and its own `MFC PUT` to write
results back; the PPU mixer polls that output. So a `bi $lr` stub that returns
without doing the PUT **hangs the game** — the mixer waits forever
(`sys_timer_usleep` spin). A correct replacement (even a passthrough) must
reproduce the **full I/O contract**: read inputs, write outputs/completion — not
just skip the compute. This is the single most important constraint for the
"generate C++" stage: model the DMA and completion, not only the arithmetic.

Corollary: these are not simple `job2` bodies where the runtime does I/O for you.
Budget the reverse-engineering per job accordingly, and lean on the oracle.

## Status / what exists

- **Interception mechanism**: shipped for the SPURS kernel (patch 0020,
  `spursHle*` in `cellSpursSpu.cpp` + `SPUThread.cpp` stop dispatch). Needs
  generalizing into the hash→handler registry above.
- **Hash identification + extraction**: working (SPU Debug → spu.log/spu-ir.log,
  ELF dumps in `spu_progs/`). DOA5 targets table above.
- **Recorded-I/O oracle**: designed here, not built. This is the highest-value
  next piece — it de-risks every subsequent HLE.
- **First real target**: mstream DSP passthrough (unblocks the in-fight
  audio-off measurement) or ATRAC→ffmpeg (biggest continuous audio win).

## Build order

1. Record-mode oracle (capture input/output pairs for a hash).
2. Generalize the stop-code interception into a hash→handler registry.
3. First HLE: mstream DSP passthrough — verify against the oracle, confirm it
   stops the deadlock, measure the in-fight FPS/contention delta.
4. ATRAC→ffmpeg HLE (the continuous audio prize).
5. Finish the SPURS kernel HLE (patch 0020) — the scheduler is the systemic win.
