# Verification record

Date: 6 October 2026. Results apply to this monitor edition, not the original archive.

| Check | Result |
| --- | --- |
| Driver x64 Debug and Release compilation/linking | Passed |
| Frontend x64 Debug and Release compilation/linking | Passed |
| Test executable x64 Debug and Release compilation/linking | Passed |
| Core protocol/ring suite | 32 checks passed in each configuration |
| Real Win32 shared-memory simulation | 13 checks passed in each configuration |
| Hidden GUI smoke test in demo mode | Startup/message loop/shutdown passed in each configuration |
| Hidden GUI smoke test without a driver | Startup/message loop/shutdown passed in each configuration |
| Driver binary format | x64 native subsystem, FORCE_INTEGRITY flag set |
| Driver loading, callbacks, live query and unload | Not executed |
| Driver Verifier and kernel stress tests | Not executed |
| Hosted GitHub Actions workflow | Added; not executed on GitHub |
| Visual interaction/layout review | Not performed by automated smoke tests |

Local complete builds used Visual Studio 2026's MSBuild and v145 compiler with the installed WDK integration and SDK/WDK headers/libraries at `10.0.26100.0`. Release compilation uses optimization and both configurations use `/W4 /WX`. The environment's VS 2022 WDK build-task integration was incomplete; VS 2026 successfully built the driver and all projects. Use a supported matching SDK/WDK/Visual Studio combination from the linked Microsoft compatibility table when reproducing the build. This record establishes local compilation, not compatibility with every Windows version.

Commands used, with the installed MSBuild path selected explicitly:

```powershell
./build.ps1 -Configuration Release -SmokeTest -MsBuildPath '<installed MSBuild.exe>'
./build.ps1 -Configuration Debug -SmokeTest -MsBuildPath '<installed MSBuild.exe>'
```

The two test suites reported:

```text
PASS: 32 checks
PASS: 13 checks
PASS: GUI startup/shutdown in demo and normal modes
```

Raw local build logs are available alongside this working copy as `build-release.log` and `build-debug.log`; logs and compiled outputs are excluded from the source ZIP and from Git. The ZIP includes source, projects, scripts, CI and documentation. Compiled files remain in the local `build` directory for convenient frontend preview.

The shared-memory suite uses the actual client against an ordinary user-mode peer. It verifies client behavior but cannot prove the kernel DACL, IRQL discipline, callback lifetime or unload path. Those require [the test VM procedure](TESTING.md). No driver was installed or loaded on the development host, and no host signing or security setting was changed.
