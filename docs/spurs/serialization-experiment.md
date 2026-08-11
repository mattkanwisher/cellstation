# "Ignore the contention by serializing" — measured (answers: were the SPUs powerful?)

Q: how fast were the SPUs, and could you dodge the contention by running them
serially on fewer host threads? Tested Max SPURS Threads = 1 (one concurrent
SPURS SPU thread) vs stock 6, in-fight on DOA5U.

| Config | writer_lock | SPU total | in-fight FPS |
|---|---|---|---|
| Stock (6) | 11.4% | 68.5% | ~39.5 |
| Reservations OFF | 10.1% | 68.1% | ~39 |
| Max SPURS = 1 | **6.5%** | 56.4% | **~21** (scene-confounded, but a real drop) |

Serializing NEARLY HALVED the writer_lock contention (proving it's inter-thread
reservation racing) — but roughly halved FPS too, because DOA5's SPU compute
(skinning/geometry/physics) genuinely needs the 6-way parallelism to hit frame
deadlines. So: the SPUs were powerful (~25.6 GFLOPS each, ~150 across the 6 game
SPUs) AND their work parallelizes productively here. You CANNOT trade the
parallelism away for the contention.

Conclusion: neither serialization (loses throughput) nor status-quo (pays the
emulation-amplified contention) is optimal. The SPURS HLE is the resolution: keep
6-way parallelism, run the scheduler host-side so the reservation traffic never
hits the emulated writer_lock — capturing both the ~39fps throughput AND the
~6.5% contention floor. That's the target of the SPURS HLE work (task #8).
