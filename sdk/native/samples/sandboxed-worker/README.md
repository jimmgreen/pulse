# Sandboxed Out-of-Process Worker Sample (`sandboxed-worker`)

Reference multi-binary C++20 Pulse plugin demonstrating the **Stage D out-of-process worker pattern**:

- **Thin in-process ABI bridge (`pulse_sandboxed_worker.dll`)**:
  - Uses only public frozen SDK interfaces (`pulse.commands`, `pulse.tasks`) plus `pulse::TaskGroup` for deterministic task collection before unload, and probes optional `pulse.ui.mount` with graceful fallback when experimental UI is absent.
  - Copies borrowed `PulseContextSelectionSnapshot` strings before returning from the command callback.
- **Job-Object-governed sidecar (`sandboxed_worker.exe`, declared in `native.files`)**:
  - Spawned suspended (`WIN32` subsystem + `DETACHED_PROCESS`, never spawning `conhost.exe`) and assigned to a Win32 Job Object (`JOB_OBJECT_LIMIT_ACTIVE_PROCESS` with `ActiveProcessLimit = 1`, `JOB_OBJECT_LIMIT_PROCESS_MEMORY` = 128 MiB, `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, `JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION`).
  - Uses `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` (inheriting only the anonymous request/response pipes) and `PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY` (`PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`).
  - Zero `USER32.dll`, `GDI32.dll`, or `SHELL32.dll` imports in both binaries (Low Integrity compatible).
  - Polls `PulseTaskContext::cancelled` cooperatively; closing the Job handle guarantees zero orphan child processes on cancellation or plugin unload.

## Offline Build, Stage & Headless Smoke Test

```powershell
# 1. Build DLL + sidecar worker (auto-discovers Visual Studio x64 toolchain via vswhere/vcvars64.bat)
python tools/extensions/pulse_ext.py build sdk/native/samples/sandboxed-worker

# 2. Bind DLL + sidecar hashes and pack .pulse-ext 100% offline (no pulse.exe required)
#    (Copy pulse_sandboxed_worker.dll and sandboxed_worker.exe into a staging folder first)
python tools/extensions/pulse_ext.py native-manifest sdk/native/samples/sandboxed-worker/manifest.json `
    --library stage/pulse_sandboxed_worker.dll `
    --file stage/sandboxed_worker.exe `
    --output stage/manifest.json
python tools/extensions/pulse_ext.py pack stage/manifest.json --output stage/sandboxed-worker.pulse-ext
python tools/extensions/pulse_ext.py verify stage/sandboxed-worker.pulse-ext

# 3. Smoke-test lifecycle + command + sidecar worker IPC headlessly in <1s
python tools/extensions/pulse_ext.py test stage/sandboxed-worker.pulse-ext --invoke-command hash-selection
```
