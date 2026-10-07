# Kernel Process Monitor

A C++ Windows driver and desktop process monitor. The driver captures process creation and exit notifications and answers read-only process queries through a **shared memory section, two events and a system worker thread**. The user-mode window provides a filterable process table and kernel event history.

This edition retains the original project's communication architecture and `vA`/`um` structure. Its operation set has been adapted for process monitoring, with a new versioned protocol. It is a development PoC, with kernel runtime validation still pending; it is not a production driver.

## What the window shows

| View | Data source |
| --- | --- |
| Process name, PID, parent PID and thread count | Windows Toolhelp snapshot, refreshed approximately once a second |
| Process creation/exit, timestamp, sequence and creation image | Driver process notification callback |
| Selected PID's creation time and exit status | Driver query using a referenced process object |
| Connection state, total creates/exits and lost events | Shared-memory response |

The GUI runs in user mode. Its event capture and selected-process query run in kernel mode. Keeping ordinary enumeration in user mode avoids relying on undocumented kernel process-list layouts. Existing processes appear in the table immediately; kernel event capture begins when the driver loads, and the frontend initially follows events from its connection time.

The frontend borrows the process-table style of diagnostic tools. It does not scan or change process memory. The previous game-specific frontend, arbitrary memory operations and concealment/spoofing code are excluded from this monitor edition. See [the migration notes](docs/MIGRATION.md) for the exact boundary of the refactor.

## Build

Install the Visual Studio C++ desktop workload and a compatible Windows SDK/WDK pair, including the WDK Visual Studio integration. The projects default to SDK `10.0.26100.0`, x64 and C++17. User projects select v143 for VS 2022 or v145 for VS 2026. Use [Microsoft's WDK compatibility table](https://learn.microsoft.com/en-us/windows-hardware/drivers/other-wdk-downloads) to choose your installation.

From a PowerShell prompt in this directory:

```powershell
./build.ps1 -Configuration Release -SmokeTest
```

This builds the driver, frontend and tests, then runs the ordinary user-mode protocol/ring and shared-memory tests. `-SmokeTest` also checks GUI startup and orderly shutdown in demo and normal modes. Binaries go into `build/x64/Release/`. The driver build is **unsigned**; building it does not install or load it.

To build the frontend and tests without a WDK:

```powershell
./build.ps1 -UserOnly -SdkVersion 10.0.26100.0 -SmokeTest
```

Supply the version of your installed SDK if it differs. `-MsBuildPath` selects a particular Visual Studio installation; `-SdkVersion` overrides the default SDK for all projects. Alternatively, open `vA.sln` in Visual Studio with the matching WDK installed. Compiler warnings at level 4 are treated as errors.

## Try the frontend

```powershell
./build/x64/Release/KpmMonitor.exe --demo
```

Demo mode shows the real Windows process list alongside **clearly labelled synthetic events**. It requires no driver and proves only the frontend can run. Without `--demo`, a missing driver produces a visible connection error and retries; the Windows process table remains usable.

For real kernel events, first follow [the test VM guide](docs/TESTING.md), then run the frontend elevated without `--demo`. Filter by executable name or PID and select a row to request its kernel details. Start and close a process to see its lifecycle events. Only one cooperative frontend can own the shared request slot at a time.

## Engineering details

- A pointer-free protocol with fixed sizes, version checks, request IDs, driver epochs, explicit NTSTATUS results and bounded payloads.
- One request slot with Interlocked state transitions; notification events replace busy polling.
- A fixed 128-record event ring protected by a spin lock; sequence cursors and explicit overwrite counts keep loss visible.
- Callback-owned image data is copied before raising IRQL; the callback does not allocate or wait under the lock.
- Startup failures propagate to `DriverEntry` and trigger cleanup. Unload unregisters callbacks, joins the worker, unmaps its view and closes handles.
- The section and events have explicit access for Administrators and SYSTEM. Named-object collisions fail startup rather than adopting an existing object.
- The client closes an uncertain channel after a 1.5-second response timeout. The GUI performs requests on its background thread.

See [architecture and limitations](docs/ARCHITECTURE.md), [verification results](docs/VERIFICATION.md) and [the VM test checklist](docs/TESTING.md). Hosted CI builds only the user-mode projects and tests; it does not claim to execute the driver.

## Portfolio description

An accurate description after reviewing and understanding the changes is:

> Windows process-monitoring PoC in C++ with a kernel driver, shared-section IPC and an event-driven desktop client; added explicit request validation, bounded event buffering, deterministic resource cleanup and automated protocol/transport tests.

Do not describe the driver as production-ready or verified under Driver Verifier until the VM checklist has been completed. Keep the original project history and acknowledge any borrowed components when publishing.
