# Receiver native regressions (Windows)

Run from the workspace after the main native app has been built:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\moonlight\tests\receiver-regression\run.ps1
```

The runner reads the existing app Makefile compiler/dependency flags, snapshots dependency object files, compiles the actual current ComputerManager and ReceiverService plus their MOC files, and links an alternate console main. All generated output stays in `artifacts/receiver-regression`; it never runs app qmake/nmake or modifies app build outputs. `-Workspace`, `-AppBuild` and `-Qt` override the default workspace/build/Qt locations. The script currently assumes the installed VS 18 Community vcvars64 path.

The executable creates temporary INI settings and a temporary stream-child marker. Control/discovery test sockets and the deliberately failing HTTP pairing endpoint bind only loopback with ephemeral ports. Compatibility downloads use an unreachable loopback proxy; mDNS/polling never starts. The child executable records `stream|UUID|Desktop` and exits, so no decoder or employee-host stream runs.

Coverage includes fresh add phase transition, certificate-only state, addressed add correlation, unrelated pair completion, a real failed pairing worker, failed cached verification, timeout and cancellation with late completion, persisted UUID/certificate before launch, UUID child arguments with duplicate addresses, asynchronous deletion, duplicate pairing rejection, deletion during an actual pairing lease, and real HTTP `/state` plus UDP discovery assertions after deletion. A UDP baseline is recorded before any host deletion, allowing post-deletion failure to be compared with the same environment before deletion. UDP delivery may fail in the execution environment; the final assertion remains visible and the runner exits nonzero. It is not silently skipped.

Limitations: private-member access exists only in the alternate test translation unit to inject deterministic asynchronous events; successful crypto handshake completion is injected, so this does not replace a real Sunshine end-to-end pairing test. The real failing worker and successful persistence/launch path execute production code. Production `start()` (tray/firewall/config discovery) is bypassed for network isolation; listeners connect directly to actual production slots. Main GUI startup and process-wide shutdown remain main-build/manual integration coverage.
