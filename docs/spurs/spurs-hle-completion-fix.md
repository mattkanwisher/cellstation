# SPURS HLE — the completion-event stall: real ABI vs the HLE, and the fix

Context: patch 0020 revives rpcs3's dormant SPURS-kernel HLE and diverts the
DOA5U kernel image (SHA1 `756df5d8…`) to C++ via reserved SPU stop codes. Both
SPURS instances schedule and the audio job chain heartbeats, but the MAIN
instance runs ~1 s and then stalls with PPU threads spinning in
`_sys_lwmutex_lock` / `_sys_lwcond_queue_wait` / `sys_event_queue_receive`,
waiting on a SPURS job-/workload-completion event that never arrives.

This document (a) transcribes the **real** kernel2 select + dispatch ABI from the
on-device disassembly, (b) shows the HLE matches it — refuting the "select
returns a wrong pollStatus/wid so the guest module skips its completion-send"
hypothesis at the ABI level, (c) identifies the actual gap (a stubbed SPU→PPU
send) and the fix, and (d) specifies the exact on-device evidence that confirms
it.

Everything below is gated behind the default-off `SPURS HLE (experimental)`
config flag.

---

## 1. The real kernel2 dispatch loop (transcribed from the disassembly)

Source: `docs/spurs/doa5u-kernel2-blocks.txt`, image hash `756df5d8…`. Addresses
are LS offsets. rpcs3 SPU register-slot convention: the SPU **preferred word**
(bytes 0–3, SPU word 0) is `gpr[n]._u32[3]`; SPU word 1 (bytes 4–7) is
`_u32[2]`; the preferred **doubleword** (bytes 0–7) is `gpr[n]._u64[1]`. Branch
targets and scalar args live in the preferred word, exactly as the HLE uses them.

### Control flow

```
0x848 kernel2 entry ─ ila sp,0x3ffd0 ; r3 = 0x20 (sys-service wid) ; brsl 0x6e8
0x6e8 dispatch loop ─ takes r3 = widAndPollStatus
        │  if workload image changed → 0x7c4: MFC GET the module to LS 0xa00
        │  ila sp,0x3ffb0                         (module stack)
        └─ 0x820 dispatch block:
               mr      r5, r11          ; r5 = pollStatus  (r11 = rotqbyi r3,4)
               lqa     r50, 0x3ffe0     ; r50 = WorkloadInfo[0..15] (addr:arg)
               ila     r3, 0x100        ; r3 = kernel-context LS ptr
               il      r11, 0xa00       ; module entry
               rotqbyi r4, r50, 8       ; r4 = arg  (bytes 8..15 → preferred dword)
               bisl    lr, r11          ; call module @0xa00, lr = 0x838
0xa00 policy module (GUEST code: real libsre) runs, returns to lr
0x838 kernel2 exit  ─ ila sp,0x3ffd0 ; fsmbi r3,0 (isPoll=0) ; brsl 0x290 (select)
0x844               ─ brsl 0x6e8       ; loop with r3 = select's return
0x290 select        ─ GETLLAR 0x80 @spurs_ea ; …choose… ; PUTLLC ; bi lr (r3=result)
```

### Register ABI handed to the policy module at 0xa00 (block 0x820)

| reg | real kernel2 | source |
|-----|--------------|--------|
| `lr`  = `gpr[0]` | `0x838` (kernel2 exit) | return addr of `bisl` |
| `sp`  = `gpr[1]` | `0x3ffb0` | `ila sp,0x3ffb0` @0x780 |
| `r3`  = `gpr[3]` | `0x100` (kernel-context LS ptr) | `ila r3,0x100` @0x828 |
| `r4`  = `gpr[4]` | `WorkloadInfo.arg` (u64, preferred dword) | `rotqbyi r4,r50,8` |
| `r5`  = `gpr[5]` | `pollStatus` (preferred word) | `mr r5,r11 = rotqbyi r3,4` |
| `pc`  | `0xa00` | `bisl lr,r11` |

`WorkloadInfo` layout (`cellSpurs.h`): `addr`@0x00 (8B), `arg`@0x08 (8B),
`size`@0x10. `rotqbyi r4,r50,8` rotates the 16-byte quad left 8 → bytes 8..15
(`arg`) land in bytes 0..7 = the preferred doubleword = `_u64[1]`.

### Select return encoding (block 0x290 → `bi lr`, consumed at 0x6e8)

The dispatch loop 0x6e8 reads the select's `r3` and derives:
- workload id from the **preferred word**: `andi r35,r3,0xf`, `clgti r32,r3,0x1f`,
  `clgti r28,r3,0xf` (bank/index selection);
- `pollStatus = r11 = rotqbyi r3, 4` → the preferred word of the result is SPU
  **word 1** of `r3` (bytes 4–7).

So the real select returns, in `r3`:
`_u32[3] = wklSelectedId`, `_u32[2] = pollStatus`, i.e.
`gpr[3]._u64[1] = (u64(wklSelectedId) << 32) | pollStatus`.

---

## 2. The HLE matches the real ABI (hypothesis refuted)

`spursKernel2SelectWorkload` (`cellSpursSpu.cpp`):

```cpp
u64 result = u64{wklSelectedId} << 32;
result |= pollStatus;
spu.gpr[3]._u64[1] = result;          // == real: wid in _u32[3], pollStatus in _u32[2]
```

`spursKernelDispatchWorkload`:

```cpp
spu.gpr[0]._u32[3] = ctxt->exitToKernelAddr; // 0x838  ✔ (real: bisl lr)
spu.gpr[1]._u32[3] = 0x3FFB0;                //        ✔ (real: ila sp,0x3ffb0)
spu.gpr[3]._u32[3] = 0x100;                  //        ✔ (real: ila r3,0x100)
spu.gpr[4]._u64[1] = wklInfo->arg;           //        ✔ (real: rotqbyi r4,r50,8)
spu.gpr[5]._u32[3] = pollStatus;             //        ✔ (real: mr r5,r11)
spu.pc = 0xA00;                              //        ✔ (real: bisl lr,r11)
```

Every register the kernel hands the guest policy module, **and** the
`(wid<<32)|pollStatus` return encoding, are byte-for-byte identical to the real
kernel2. The audio job chain (a guest module reached through this same path)
runs correctly, which independently confirms the ABI is right. **Conclusion: the
prime hypothesis — that a wrong select result makes the guest job-queue module
branch wrong and skip its completion-send — is refuted at the ABI level.** The
`pollStatus` *bit composition* (READYCOUNT=1 / SIGNAL=2 / FLAG=4) is the standard
documented SPURS module-poll ABI and the HLE composes it per the reconstruction;
DOA5U leaves `wklFlag` unused (receiver 0xFF), so only READYCOUNT/SIGNAL are ever
in play, and both instances observably schedule.

The stall is therefore **not** a mis-encoded scheduler decision. It is a missing
**notification** on the SPU→PPU completion path.

---

## 3. The actual gap: a stubbed SPU→PPU completion send

`spursSysServiceUpdateShutdownCompletionEvents` (`cellSpursSpu.cpp`) computes
`wklNotifyBitSet` — the set of workloads that have finished shutting down **and**
have a completion hook registered (`wklEvent1[i] & 0x02 || & 0x10`) — and then:

```cpp
if (wklNotifyBitSet)
{
    // TODO: sys_spu_thread_send_event(spuPort, 0, wklNotifyMask);   // <-- never sent
}
```

The send was left as a bare TODO. The LLE kernel performs this
`sys_spu_thread_send_event` unconditionally at this point; it is the SPU→PPU half
of the workload-completion handshake. Without it, a guest PPU thread that
attached an lv2 event queue to the workload (`cellSpursWorkloadAttachLv2EventQueue`)
and is blocked in `sys_event_queue_receive` is never woken — exactly the observed
stall. The sibling path
`spursSysServiceCleanupAfterSystemWorkload` already issues the analogous send
(`sys_spu_thread_send_event(spu, spurs->spuPort, 2, 0)`), so the handshake,
port, and mailbox protocol are known-good in this context.

### The fix (this patch)

```cpp
if (wklNotifyBitSet)
{
    sys_spu_thread_send_event(spu, spuPort, 0, wklNotifyBitSet);
}
```

`spuPort` = the SPURS-instance event port (`spurs->spuPort`), `data0 = 0` selects
the shutdown-completion event class, `data1 = wklNotifyBitSet`. `spuPort` lost its
`[[maybe_unused]]` now that it is read. The call only fires when a workload has
actually transitioned SHUTTING_DOWN → REMOVABLE with a hook registered, matching
LLE behaviour, so it cannot spuriously block the sys-service loop.

**Honesty on confidence.** This is the one concrete missing completion-send I can
prove is wrong by inspection, and it is squarely on the completion path. It fires
on workload teardown/reconfiguration, not necessarily on every per-frame job
completion. If DOA5U's per-frame completion runs through the **taskset** PM
(`spursTasksetProcessRequest` DESTROY/OnTaskExit) or through a guest **job-queue**
module's own syscall rather than the sys-service shutdown path, the starving queue
will point there instead — which the instrumentation below resolves without
guessing.

---

## 4. On-device evidence that confirms (or redirects) the fix

Patch 0020 adds flag-gated, rate-limited diagnostics in `sys_event.cpp`:

- `sys_event_queue_receive`: logs `equeue_id` (+ how hot it is). Under the stall,
  one queue dominates this at ~470/s — that is the queue the PPU is starving on.
- `sys_event_port_send`: logs the **target queue id** each send reaches
  (`port.queue->id`) + `data1` + whether it woke a thread.

Both are `[[unlikely]]` behind `g_cfg.core.spurs_hle` and emit nothing when the
flag is off.

### The experiment (A/B)

1. **HLE OFF** (stock LLE), boot DOA5U into a fight; capture a window of the two
   diagnostics. Record the set of queue ids that receive sends and the id the
   PPU receives on. This is the ground truth: which queue carries the completion
   event, and which port/side sends it.
2. **HLE ON**, same scene. The starving queue is the `equeue_id` that dominates
   `queue_receive` but appears as a `port_send` **target** far less often (or
   never) than under HLE-off.

### Expected vs actual

- **If the fix is correct:** with the fix applied, HLE-on now shows a
  `port_send -> queue=<Q>` for the same queue id `<Q>` the PPU receives on, at the
  cadence workloads shut down, and the `queue_receive` spin on `<Q>` clears. `<Q>`
  is the SPURS event queue bound via `cellSpursWorkloadAttachLv2EventQueue`.
- **If it redirects:** HLE-on still shows no `port_send` to the starving `<Q>`
  even with the fix. Then the owed send is not the shutdown-completion but the
  taskset/job-queue completion; follow `spursTasksetProcessRequest`
  (DESTROY_TASK / OnTaskExit) and the guest job-queue module's own event syscall
  for the missing `sys_spu_thread_send_event` / `sys_event_port_send`, using the
  ground-truth queue id from step 1 as the target to match.

Either way the A/B pins the exact queue id and the side that owes the send, so the
next iteration is targeted rather than speculative.

---

## 5. What was ruled out (do not re-derive)

- Select/dispatch **register ABI** and the `(wid<<32)|pollStatus` **return
  encoding**: verified identical to the real kernel2 (§1–§2). Not the bug.
- `wklFlag`: DOA5U sets no receiver (0xFF); the FLAG poll bit is never taken.
- Reservation atomicity of the select RMW: fixed in 0020 (wrapped in
  `vm::reservation_op<Ack=true>`, publish-on-change).
- Ready-count quads: the real kernel2 never writes them; the HLE select doesn't
  either.

## On-device test result (2026-08-12) — fix inert, but diagnostics pinned the queue

Built + tested on the Thor with SPURS HLE on. HLE engages, both instances
schedule (~30 dispatches in the first second), then the MAIN instance
(spurs=0x24d9980) goes silent at ~1s while only the audio instance
(0x24dba00) heartbeats — the SAME ~1s stall. Game frozen: FPS ~5, SPU 0.0%,
black screen.

**The workload-shutdown completion send fired 0 times** — so the filled-in
`sys_spu_thread_send_event` on the shutdown path is inert for THIS stall; the
missing completion is a PER-FRAME job path, not workload teardown (the agent's
"if it redirects" case).

**The event-queue diagnostics pinned the starving queue precisely:**
- `KtslMsUpdater` (the game's MultiStream/engine updater) blocks FOREVER
  (timeout=0) on **equeue_id=0x8d01d300** — the prime starving queue.
- `_gcm_intr_thread` waits on 0x8d01a800 (RSX interrupt — likely normal vblank wait).
- `KtslAudioPort` on 0x8d01d100 has a 100ms timeout (not truly stuck).

**Next lead (task #8):** the HLE must signal 0x8d01d300 when the game's
per-frame workloads/jobs complete. Trace who sends to 0x8d01d300 under stock
(HLE-off): it's the SPURS event the taskset/job-chain PM (or the kernel's
per-workload event-flag/port path) raises on job completion — NOT the
workload-shutdown path. Implement that send in the HLE's per-workload
completion, keyed to the port the game registered (the diag already logs the
equeue_id; add the matching port_send side to find who raises it under LLE).
