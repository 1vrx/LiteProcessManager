# Verification and test VM procedure

## Tests without a driver

```powershell
./build.ps1 -UserOnly -SdkVersion 10.0.26100.0 -SmokeTest
```

The core tests cover malformed requests, unsupported versions, invalid cursors, overwritten-history accounting, independent cursors and concurrent producer/reader operation under the caller-held-lock contract. The transport suite creates its own `Local\KpmTest*` section/events in user mode and exercises the real client: request correlation, operation failure, frontend exclusion, malformed response length, wrong response IDs, timeout and missing objects. It does not emulate kernel scheduling, kernel object ACLs, callback delivery or driver unloading.

GUI smoke tests open hidden windows, run the message loop briefly and close them. They validate startup/shutdown, not visual appearance or every interaction. Hosted CI runs the user-mode suites without GUI smoke tests. It does not install WDKs or load drivers.

## Real driver testing

Use a disposable Windows x64 test VM, a snapshot and a kernel debugger. Build the driver with your matching SDK/WDK. Follow Microsoft's [driver test-signing guide](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/windows-driver-signing-tutorial) and [target provisioning procedure](https://learn.microsoft.com/en-us/windows-hardware/drivers/gettingstarted/provision-a-target-computer) to sign and prepare the **test VM**. The project deliberately leaves signing disabled in its build settings; an unsigned successful build is not a loadable release.

Copy the signed `KpmDriver.sys`, `KpmMonitor.exe`, `KpmTests.exe` and any required C++ runtime to the VM. In an elevated terminal there, with the signed file at `C:\Kpm\KpmDriver.sys`:

```powershell
sc.exe create KpmMonitor type= kernel start= demand binPath= C:\Kpm\KpmDriver.sys
sc.exe start KpmMonitor
C:\Kpm\KpmTests.exe --live
C:\Kpm\KpmMonitor.exe
```

These are manual deployment commands for the VM, not actions performed by the build script. If loading fails, inspect the SCM/Code Integrity event logs and kernel debug output. Callback registration requires the image integrity flag as well as a valid test-signing setup.

The `--live` suite queries its own PID, rejects a zero PID and unsupported protocol, checks continued service after malformed requests, and starts a short-lived child to verify both creation and exit records. Run it on a quiet VM: extreme event churn can overwrite the controlled test's records.

## Manual checklist

| Scenario | Expected result |
| --- | --- |
| Start and close a process with frontend connected | Ordered creation and exit events with its PID |
| Filter by executable or PID and select a row | Windows table filters; kernel details update |
| Start a second elevated frontend | Visible busy-channel error; first frontend remains functional |
| Launch frontend without elevation | Section/event access denied, with a visible error |
| Stop driver while frontend is open | Frontend detects disconnect; ordinary Windows table remains available |
| Close frontend, stop and reload driver, reconnect | New epoch and fresh counters, no stale event cursor |
| Rapidly start/exit processes while reader falls behind | Total counters rise; overwritten events are reported |
| Repeat load/connect/disconnect/unload cycles | No surviving worker, mappings or driver handle growth |
| Force a named section/event collision in the VM | DriverEntry fails and rolls back acquired resources |
| Terminate client during a request, then reconnect | Completed slot can recover; unresolved slot times out without unsafe reuse |

Enable [Driver Verifier](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/driver-verifier) for `KpmDriver.sys` only in the test VM, using standard settings plus checks appropriate to the WDK/OS. Repeat the lifecycle and churn tests with WinDbg attached. Record OS/WDK versions, duration, completed cycles, debugger output and any violations. This is required before claiming runtime reliability.

After testing, close the frontend before reloading or deleting the service:

```powershell
sc.exe stop KpmMonitor
sc.exe delete KpmMonitor
```

Reset Verifier through `verifier /reset` and reboot the test VM when done. Keep the VM snapshot as the recovery path.
