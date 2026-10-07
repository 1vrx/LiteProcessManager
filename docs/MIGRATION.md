# Changes from the original archive

This is a monitor edition of `kernel-proc-manager`. The communication design is preserved, but these source files contain substantial implementation changes; it is not a binary-compatible patch to the original driver.

| Original element | Monitor edition |
| --- | --- |
| `vA.sln`, `vA` driver and `um` client | Same division and solution/project identities, plus a test project |
| Named shared section, request/response events, system worker | Preserved, with a one-page bounded mapping, explicit DACLs and auto-reset events |
| Mutable, loosely defined request fields | Shared pointer-free version 2 ABI, copied requests, IDs, epochs, states, validation and statuses |
| Infinite communications worker | Event wait, stop flag, worker join and mapping/handle cleanup |
| Startup routines whose failures could be ignored | Propagated NTSTATUS and rollback through `DriverEntry` |
| Game offsets and cheat frontend | Native process table, filter, lifecycle history and selected-process details |
| Memory read/write, CR3 and physical-memory operations | Excluded from this read-only monitor edition |
| Thread hiding, return spoofing and mapper assumptions | Excluded; normal signed-driver loading and normal unload callback |
| No automated verification | Ring/protocol tests, real Win32 shared-memory simulation, optional live-driver tests and user-mode CI |

`vA/Entry.cpp` now owns initialization and unload. `vA/com/sharedmemory.cpp` owns IPC resources and worker execution. `vA/monitor` adds process callbacks and referenced-object queries. `shared` defines the contract and bounded event ring. `um/com` owns client resources, while `um/main.cpp` contains the monitor window. The original raw-memory, game and bypass files are not carried forward as dormant build options.

Why some driver lines changed: explicit shutdown requires normal driver lifecycle ownership; bounded transport requires a new ABI; process events need a callback and buffer. Shared memory was retained rather than replacing communications with IOCTLs. The goal is a reviewable kernel-development PoC with the original transport idea, adapted to the agreed monitoring frontend.
