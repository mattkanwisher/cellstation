# Cheap config levers for the SPU/scheduler cost (measured)

Low-hanging, code-free knobs tested in-fight on DOA5U (AYN Thor), vs the stock
in-fight baseline (writer_lock 11.4%, SPU 68.5%).

| Lever | Result | Verdict |
|---|---|---|
| **Accurate SPU Reservations: false** | writer_lock 11.4→10.1%, mfc 3.3→2.9%; DOA5 stable through a full fight | Small real win (~1.3pt). Cheapens each GETLLAR/PUTLLC but not the structural 6-thread-on-one-line serialization. Per-game opt-in — needs longer soak (this flag can cause subtle desyncs over time); not a safe global default. |
| SPU XFloat Accuracy | already **Approximate** (fast) | no fruit — already optimal for the float geometry kernels |
| SPU Block Size | currently **Safe** | untested; Mega/Giga = bigger recompiler blocks, may help geometry kernels — worth an A/B |
| Accurate SPU DMA | already **false** (fast) | no fruit |

**Takeaway:** config levers shave ~1-2 points at most. The reservation contention
is structural (6 SPU threads serializing on one CellSpurs line), so the real fix
is the SPURS-kernel HLE (remove the polling) or finer range-lock granularity —
not a knob. Confirms the scheduler, not config, is where the contention win lives.
