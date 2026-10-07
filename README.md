# Kernel Process Monitor

A Windows kernel-mode process monitoring project written in C++.

Kernel Process Monitor combines a Windows driver with a desktop frontend to observe process lifecycle activity and perform limited, read-only process queries. The driver captures process creation and termination notifications and communicates with the user-mode application through a shared memory section, synchronization events, and a system worker thread.

The project is intended as a **systems programming and defensive-security development PoC**. It demonstrates Windows kernel callbacks, kernel/user-mode IPC, synchronization, fixed-size protocols, process-object handling, lifecycle management, and defensive error handling.

> **Status:** Development / proof of concept.  
> Kernel runtime validation is still in progress. This is not intended for production deployment.

---

## Overview

The project consists of two main components:

- **Kernel driver (`vA`)**  
  Registers for process notifications, records lifecycle events, services process queries, and manages the shared-memory communication channel.

- **User-mode monitor (`um`)**  
  Displays the current Windows process list, consumes driver-generated events, allows filtering by executable name or PID, and requests additional information about selected processes.

Ordinary process enumeration remains in user mode using documented Windows APIs. The kernel component is reserved for information that specifically benefits from kernel-side observation.

This avoids depending on undocumented kernel process-list structures and keeps the driver's responsibilities deliberately narrow.

---

## Features

### Process monitoring

The driver records:

- Process creation
- Process termination
- Process ID
- Parent process ID
- Creation image
- Event timestamp
- Monotonic event sequence number

Existing processes are populated through a normal user-mode snapshot. Kernel event capture begins when the driver is loaded.

### Process queries

For a selected PID, the frontend can ask the driver for read-only information obtained through a referenced process object, including:

- Process creation time
- Process exit status

The monitor does **not** read, write, scan, patch, or otherwise modify process memory.

### Desktop frontend

The GUI provides:

- Live process table
- Executable-name filtering
- PID filtering
- Kernel process-event history
- Selected-process details
- Driver connection state
- Total process creation count
- Total process exit count
- Event-loss statistics

The process table is refreshed approximately once per second.

---

## Architecture

```text
+--------------------------------------+
|          User-Mode Frontend          |
|                                      |
|  Toolhelp process enumeration        |
|  Process table / event history       |
|  Filtering and process selection     |
+------------------+-------------------+
                   |
                   | Shared section
                   | Request/response slot
                   | Synchronization events
                   v
+------------------+-------------------+
|             Kernel Driver            |
|                                      |
|  Process notification callback       |
|  Fixed-size event ring               |
|  System worker thread                |
|  Read-only process queries           |
+--------------------------------------+
```

Communication intentionally retains the shared-memory design of the original codebase rather than replacing it with a conventional IOCTL interface.

The channel consists of:

- One shared memory section
- Two synchronization events
- One request/response slot
- A fixed-size process-event ring
- One kernel system worker thread

Only one cooperative frontend is expected to own the request slot at a time.

For a more detailed description, see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

---

## Protocol

The kernel/user-mode interface uses a versioned, pointer-free protocol.

Design properties include:

- Fixed-width structures
- No kernel pointers exposed to user mode
- Protocol version validation
- Request IDs
- Driver epoch identifiers
- Explicit `NTSTATUS` results
- Bounded payload sizes
- Interlocked request-state transitions
- Explicit event-loss reporting

The shared request slot uses atomic state transitions rather than polling continuously.

Synchronization events notify either side when work becomes available.

---

## Event Ring

Process lifecycle events are stored in a fixed **128-record ring buffer**.

The ring uses:

- Spin-lock protection
- Monotonic sequence numbers
- Per-client sequence cursors
- Explicit overwrite accounting

If the consumer falls behind far enough for records to be overwritten, that loss is exposed rather than silently ignored.

Image information received by the process callback is copied while it is valid. The callback avoids waiting or performing allocation while holding the event-ring lock.

---

## Build Requirements

The default configuration targets:

- Windows x64
- C++17
- Windows SDK `10.0.26100.0`
- Visual Studio 2022 or newer
- MSVC `v143` / compatible newer toolset
- Windows Driver Kit (WDK)

Install the **Desktop development with C++** workload and a compatible Windows SDK/WDK combination.

Microsoft publishes the current SDK/WDK compatibility information here:

https://learn.microsoft.com/en-us/windows-hardware/drivers/other-wdk-downloads

Warnings are enabled at level 4 and treated as errors.

---

## Building

From PowerShell in the repository root:

```powershell
./build.ps1 -Configuration Release -SmokeTest
```

This builds:

- Kernel driver
- Desktop frontend
- User-mode tests

It then runs the protocol, ring-buffer, shared-memory, and frontend smoke tests.

Build output is placed in:

```text
build/x64/Release/
```

### User-mode-only build

The frontend and tests can be built without installing the WDK:

```powershell
./build.ps1 -UserOnly -SdkVersion 10.0.26100.0 -SmokeTest
```

If your installed Windows SDK differs, provide its version with `-SdkVersion`.

A specific Visual Studio installation can also be selected using:

```text
-MsBuildPath
```

Alternatively, open:

```text
vA.sln
```

in Visual Studio with an appropriate WDK installed.

---

## Demo Mode

The frontend can be exercised without loading the driver:

```powershell
./build/x64/Release/KpmMonitor.exe --demo
```

Demo mode displays the real Windows process table alongside **clearly labelled synthetic kernel events**.

This is useful for testing:

- GUI startup
- Process enumeration
- Filtering
- Event rendering
- Selection behaviour
- Clean application shutdown

Demo mode does **not** simulate or prove correct kernel execution.

---

## Running With the Driver

The driver build is unsigned.

Building the project does **not** automatically:

- Install the driver
- Register a service
- Modify boot configuration
- Load the driver

Kernel testing should be performed in an isolated Windows test VM.

See:

[`docs/TESTING.md`](docs/TESTING.md)

for the test environment and validation checklist.

Once the driver has been loaded in the test environment, start the frontend elevated without `--demo`:

```powershell
./build/x64/Release/KpmMonitor.exe
```

You can then:

1. Filter processes by executable name or PID.
2. Select a process to request kernel-side information.
3. Start another application.
4. Observe its creation event.
5. Close it.
6. Observe its termination event.

If the driver is unavailable, the frontend reports the connection failure and retries while keeping the ordinary Windows process table usable.

---

## Reliability and Cleanup

Driver initialization is treated transactionally.

If a required initialization stage fails, the failure propagates back to `DriverEntry` and resources acquired earlier in startup are released.

During unload, the driver:

- Stops accepting new work
- Unregisters process callbacks
- Signals the worker thread
- Waits for the worker to terminate
- Unmaps the shared section
- Closes kernel handles
- Releases remaining resources

Named shared objects are created with explicit access for **Administrators** and **SYSTEM**.

Unexpected named-object collisions cause initialization to fail instead of silently attaching to an existing object.

On the user-mode side, a request that does not receive a response within approximately **1.5 seconds** causes the client to treat the communication channel as uncertain and close it rather than assuming that shared state remains valid.

GUI requests are handled from a background thread so the window does not block on kernel communication.

---

## Verification

The repository includes user-mode tests covering the parts of the protocol that can be exercised without loading kernel code.

These include:

- Protocol validation
- Version handling
- Ring-buffer behaviour
- Sequence handling
- Event overwrite accounting
- Shared-memory state transitions
- Frontend startup/shutdown smoke tests

Hosted CI builds and tests the user-mode components.

It does **not** claim to execute or validate the Windows kernel driver.

See [`docs/VERIFICATION.md`](docs/VERIFICATION.md) for the current verification status.

---

## Scope and Non-Goals

Kernel Process Monitor is intentionally limited in scope.

It does not implement:

- Arbitrary process memory reads
- Arbitrary process memory writes
- Code injection
- Process manipulation
- Process concealment
- Handle hijacking
- Kernel structure patching
- Anti-cheat bypasses
- Identity or hardware spoofing

Its kernel interface is limited to process lifecycle monitoring and read-only process metadata queries.

---

## Project Background

This project evolved from an earlier personal Windows kernel/user-mode experimentation codebase.

The original project was built while learning driver development and was previously used as a foundation for game-security and game-cheat experimentation. For this version, I repurposed the architecture into a defensive process-monitoring project suitable for exploring Windows internals, endpoint visibility, and kernel/user-mode communication.

The refactor deliberately preserves much of the original infrastructure, including the `vA` / `um` project structure and shared-memory communication model, while removing game-specific behaviour, arbitrary memory operations, and concealment/spoofing functionality.

AI-assisted development was used during the process-monitoring refactor, particularly to help adapt and review portions of the codebase. The underlying project architecture and a substantial portion of the implementation originate from my earlier work.

For a more precise description of what changed, see:

[`docs/MIGRATION.md`](docs/MIGRATION.md)

---

## Why This Project Exists

The aim of the project is to explore several areas of Windows systems engineering in one relatively small codebase:

- Windows kernel driver development
- Process notification callbacks
- Kernel synchronization
- Shared-memory IPC
- Locking and IRQL constraints
- Kernel object lifetime management
- Defensive protocol design
- Failure-path cleanup
- User/kernel trust boundaries
- Desktop diagnostic tooling
- Testable separation between kernel and user-mode functionality

It is primarily a learning and portfolio project rather than an attempt to replace established tools such as Process Explorer, Process Monitor, or production EDR telemetry.

---

## Repository Documentation

Additional documentation is available in:

```text
docs/
├── ARCHITECTURE.md   # Driver/frontend architecture and limitations
├── MIGRATION.md      # Boundary between the original and monitor versions
├── TESTING.md        # Test-VM setup and kernel validation checklist
└── VERIFICATION.md   # Current verification and test results
```

---

## Security Notice

This project contains Windows kernel-mode code.

A bug in a kernel driver can cause system instability, data loss, or a system crash. Do not test development drivers on a machine containing important data.

Use an isolated virtual machine with appropriate debugging and recovery facilities.

The driver is provided for educational, research, and portfolio purposes and is **not production-ready**.
