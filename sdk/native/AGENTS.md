# Pulse Native SDK — Autonomous Agent Playbook (`AGENTS.md`)

High-density reference for AI coding agents (Antigravity, Claude Code, Cursor, Codex) authoring Windows x64 C++20 plugins against the Pulse Native SDK (`PULSE_SDK_PREVIEW = 3`, `PULSE_ABI_VERSION_1 = 1`).

- **Source of truth**: Public C headers in [`include/pulse/`](include/pulse/abi.h) and machine metadata in [`api-catalog.json`](api-catalog.json).
- **Agent skill & MCP config**: [`skills/pulse-plugin-author/SKILL.md`](skills/pulse-plugin-author/SKILL.md) and [`mcp.example.json`](mcp.example.json).
- **Out-of-process worker reference**: [`samples/sandboxed-worker/`](samples/sandboxed-worker/README.md) (Stage D Win32 Job Object + framed pipe sidecar).

---

## 1. Five-Command Autonomous Loop (`new -> build -> test -> stage -> install`)

`tools/extensions/pulse_ext.py` works **100% offline** (no `pulse.exe` GUI required) and **auto-discovers** MSVC `vcvars64.bat`, `cmake`, and `ninja` via `vswhere.exe`. Run from the repository root or extracted SDK root:

```powershell
# 1. Scaffold from one of the 8 packable templates (never overwrites existing files)
python tools/extensions/pulse_ext.py --json new C:/Work/my-plugin --id com.acme.my-plugin --name "Acme Plugin" --template inspector

# 2. Build x64 DLL (auto-discovers MSVC vcvars64.bat + CMake + Ninja; zero pre-activated prompt needed)
python tools/extensions/pulse_ext.py --json build C:/Work/my-plugin --config Release

# 3. Smoke-test headlessly (<1s): validates manifest, PE64 exports, ABI lifecycle, commands & clean unload
python tools/extensions/pulse_ext.py --json test C:/Work/my-plugin --smoke-commands --strict-resources

# 4. Stage: bind DLL (+ optional --file sidecars) SHA-256 into manifest, pack .pulse-ext, and verify
python tools/extensions/pulse_ext.py --json native-manifest C:/Work/my-plugin/manifest.json --library C:/Work/my-plugin/build/com_acme_my_plugin.dll --output C:/Work/my-plugin/build/manifest.json
python tools/extensions/pulse_ext.py --json pack C:/Work/my-plugin/build/manifest.json --output C:/Work/my-plugin/build/com.acme.my-plugin.pulse-ext
python tools/extensions/pulse_ext.py --json verify C:/Work/my-plugin/build/com.acme.my-plugin.pulse-ext

# 5. Install & enable into an isolated test store (never touch %LOCALAPPDATA%\Pulse during automated tests)
python tools/extensions/pulse_ext.py --json install C:/Work/my-plugin/build/com.acme.my-plugin.pulse-ext --store C:/Work/test-store --allow-native --accept-capabilities
python tools/extensions/pulse_ext.py --json enable com.acme.my-plugin --store C:/Work/test-store --allow-native
```

### Equivalent MCP Server Loop (`tools/extensions/native_author_mcp.py`)

1. `native_sdk_describe` — inspect interfaces, `templates` (8 packable), `abiSamples`, and `referenceSamples` (`archive`, `sandboxed-worker`).
2. `native_project_create` — scaffold `projects/<name>` with collision-free `id` and CMake target names.
3. `native_project_build` — auto-discover MSVC/CMake/Ninja and compile (`Release`, `RelWithDebInfo`, or `Debug`).
4. `native_plugin_test` — run offline manifest + PE64 checks and headless lifecycle/command smoke tests (`pulse_plugin_harness.exe`).
5. `native_package_stage` -> `native_package_install` — produce verified `.pulse-ext` and install into `<workspace>/.pulse-author/extensions`.
6. Optional live host check: `native_host_diagnose` -> `native_host_run` -> `native_host_logs` -> `native_host_stop`.

---

## 2. C ABI Golden Rules (Non-Negotiable)

1. **Required exports**: Every primary plugin DLL must declare `PULSE_PLUGIN_SDK_PREVIEW()` at file scope (exports `PulsePluginSdkPreview` returning `3`) and `extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out)`.
2. **Never throw C++ exceptions across `PULSE_CALL` boundaries**: Wrap every callback (`activate`, `deactivate`, `destroy`, command/task/preview/data/event callbacks) in `try { ... } catch (...) { return PULSE_ERROR_INTERNAL; }`, or use the RAII wrappers in `pulse/client.hpp` (`CommandRegistration`, `TaskGroup`, `DispatchQueue`, `Subscription`).
3. **Strict `size` / `version` negotiation**: Check `host != nullptr && out != nullptr && host->size >= sizeof(PulseHostV1) && host->abi_version == PULSE_ABI_VERSION_1` and `out->size >= sizeof(PulsePluginV1)`. Set `out->size = sizeof(PulsePluginV1)` and initialize every queried descriptor's `.size = sizeof(...)` before calling host functions.
4. **Borrowed `PulseStringView` lifetime**: Every `PulseStringView` (`{const char* data, size_t size}`, UTF-8, **not guaranteed NUL-terminated**) passed into a plugin callback is **borrowed only until that callback returns**. Copy into `std::string` before capturing in async tasks or storing in plugin state. Conversely, views passed *to* synchronous-copy host APIs (`model.create`, `data.publish`, `tasks.report`, `preview->publish`) need only outlive that call.
5. **No cross-CRT ownership**: Never `free` / `delete` memory allocated by the host, and never pass `std::string` / `std::vector` across the C ABI.
6. **UI-thread affinity vs worker threads**:
   - `PulsePluginEntryV1`, `activate`, `deactivate`, `destroy`, `host->query`, and all `pulse.ui.*`, `pulse.commands`, `pulse.context`, `pulse.directories`, `pulse.files`, `pulse.fileops`, `pulse.resources`, `pulse.settings.pages`, and `pulse.dialogs` calls run **exclusively on the owner UI thread**. Never block the UI thread with file I/O, hashing, compression, or `WaitForSingleObject`.
   - Offload heavy work to `pulse::sdk::TaskGroup` (`pulse.tasks` host workers) or a Win32 Job-Object-governed helper `.exe` (see [`samples/sandboxed-worker/`](samples/sandboxed-worker/README.md)).
   - From background threads, **only** `pulse.dispatch` (`post`/`cancel`), `pulse.ui.data` (`publish`), `pulse.tasks` (`context->report` / `context->cancelled` before `work` returns), and `pulse.staging` (`create`/`commit`/`get`/`cancel`/`release`) are legal.
   - Before `destroy` returns, join all plugin-owned threads, release completed tasks (`TaskGroup::Collect`), unregister providers/commands, and close open file/process handles so `FreeLibrary` unloads cleanly (`unloadClean: true`).
7. **`frozen` vs `experimental` stability fallback**:
   - **12 `frozen` interfaces** (`pulse.files`, `pulse.resources`, `pulse.dispatch`, `pulse.directories`, `pulse.context`, `pulse.fileops`, `pulse.commands`, `pulse.preview`, `pulse.tasks`, `pulse.staging`, `pulse.settings.pages`, `pulse.dialogs`) + core entry ABI are binary-frozen for 1.x.
   - **4 `experimental` UI interfaces** (`pulse.ui.model`, `pulse.ui.events`, `pulse.ui.data`, `pulse.ui.mount`) may return `PULSE_ERROR_UNAVAILABLE` (`std::nullopt` in `Host::Query`). Command/task/preview plugins must degrade gracefully when experimental UI is absent (test with `pulse_ext.py test --omit-experimental-ui`).
8. **Out-of-process isolation for untrusted parsers (`samples/sandboxed-worker`)**:
   - In-process DLLs share Pulse's address space and user privileges (`LoadLibraryExW` runs `DllMain` before `PulsePluginSdkPreview` is checked). Whenever parsing untrusted third-party formats or isolating crash/memory risk, keep the DLL as a thin `pulse.commands` + `pulse.tasks` bridge and spawn a sidecar `.exe` (bundled via `native.files`) inside a Win32 Job Object (`ActiveProcessLimit = 1`, `ProcessMemoryLimit`, `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, `PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`, explicit `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` pipe inheritance, zero `USER32`/`GDI32`/`SHELL32` imports).

---

## 3. Manifest Rules & `resources` Mapping Table

`manifest.json` must be strict UTF-8 JSON (no BOM, no duplicate keys, `<= 65536` bytes) containing only `apiVersion` (`1`), `id` (`1..64` ASCII `[A-Za-z0-9._-]`, never `sample.*` or Windows device names), `name` (`<= 128` UTF-8 bytes), optional `resources` (unique subset of the 15 values below), optional `contributes.commands` (`<= 32` static zero-load menu/palette entries), and `native` (injected via `pulse_ext.py native-manifest`).

| Queried SDK Interface / Operation | Required `manifest.json` `resources` Declaration(s) | Stability Tier |
|---|---|---|
| `pulse.context` (`get`, `get_selection`) | `context.read` | `frozen` |
| `pulse.context` (`subscribe`) | `context.read`, `context.events` | `frozen` |
| `pulse.files` (`current_directory`, `list_entries`) | `files.readCurrentDirectory` | `frozen` |
| `pulse.files` (`prepare_rename`, `commit_rename`) | `files.readCurrentDirectory`, `files.renameCurrentDirectory`, `operations.own` | `frozen` |
| `pulse.files` (`task_get`, `task_cancel`) / `pulse.tasks` / `pulse.staging` | `operations.own` | `frozen` |
| `pulse.directories` (`open`, `refresh`, `list`) | `files.readDirectory` | `frozen` |
| `pulse.directories` (`watch`, `unwatch`) | `files.readDirectory`, `files.watchDirectory` | `frozen` |
| `pulse.directories` (`open_entry`) | `files.readDirectory`, `files.open` | `frozen` |
| `pulse.fileops` (`submit` copy/move) | `files.transferCurrentSelection`, `operations.own` | `frozen` |
| `pulse.fileops` (`submit` recycle) | `files.deleteCurrentSelection`, `operations.own` | `frozen` |
| `pulse.fileops` (`submit_to_directory`) | `files.transferToDirectory`, `files.readDirectory`, `operations.own` | `frozen` |
| `pulse.fileops` (`submit_entries`) | `files.transferFromDirectory`, `files.transferToDirectory`, `files.readDirectory`, `operations.own` | `frozen` |
| `pulse.fileops` (`submit_directory_tree`) | `files.transferFromDirectory`, `files.transferToDirectory`, `files.readDirectory`, `files.transferDirectoryTree`, `operations.own` | `frozen` |
| `pulse.resources` (`stat`) | `files.readDirectory` (or `files.readCurrentDirectory`) | `frozen` |
| `pulse.resources` (`settings_get`, `settings_set`) / `pulse.settings.pages` | `settings.own` | `frozen` |
| `pulse.resources` (`audit_list`) | `audit.own` | `frozen` |
| `pulse.commands`, `pulse.preview`, `pulse.dispatch`, `pulse.dialogs`, `pulse.ui.*` | *(none required by interface alone; declare resources for underlying data/ops)* | `frozen` (`pulse.ui.*` = `experimental`) |

---

## 4. Packable Templates vs Reference Samples

- **9 Packable `new` Templates** (`examples/native/catalog.json`):
  - `archive-dialog` — 7-Zip-style host dialog (`pulse.dialogs` V2: heading, icon, subject card, footnote), derived form states, folder picker via `pulse.pickers`.
  - `inspector` — directory stats, CAS settings (`pulse.resources`), audit view.
  - `rename-panel` — two-phase rename plan + host confirmation (`pulse.files`).
  - `reactive-panel` — `std::jthread` worker + `pulse.dispatch` + `pulse/client.hpp`.
  - `live-data` — paged & delta `pulse.ui.data` provider (`virtualList` / `dataTable` / `tree`).
  - `directory-browser` — read-only paged `pulse.directories` + `pulse.ui.data`.
  - `file-operations` — selection copy/move/recycle via `pulse.fileops`.
  - `directory-transfer` — explicit destination directory & tree copy (`PulseFileOpsV4`).
  - `directory-workbench` — dockable/windowed `pulse.ui.mount` + dual-directory transfer.
- **Reference Samples** (inspect/copy directly; not `pulse_ext.py new` templates):
  - [`samples/sandboxed-worker/`](samples/sandboxed-worker/README.md) — Stage D out-of-process worker (`pulse.commands` + `pulse.tasks` + Job Object + pipe IPC + `native.files` sidecar `.exe`).
  - [`samples/archive/`](samples/archive/plugin.cpp) — multi-file archive preview, extraction staging, settings page, and 7z/ZIP writer.

---

## 5. Rapid Troubleshooting Table

| Error Code / Symptom | Root Cause | Immediate Fix |
|---|---|---|
| `PULSE_ERROR_SDK_PREVIEW` / `abi_exports_missing` | Missing `PULSE_PLUGIN_SDK_PREVIEW()` macro or compiled against old `abi.h` | Include `<pulse/sdk.hpp>`, place `PULSE_PLUGIN_SDK_PREVIEW()` at file scope, rebuild x64 DLL. |
| `toolchain_not_ready` | MSVC x64 / Windows SDK / CMake / Ninja missing | Run `pulse_ext.py --json doctor --toolchain` to inspect `missingReasons`; install VS C++ x64 workload. |
| `reserved_id` / `invalid_manifest` | `id` starts with `sample.`, uses Windows device name, or legacy manifest keys (`runtime`, `commands`, `panel`) | Use reverse-DNS `com.org.feature`; put static commands under `contributes.commands`, remove legacy keys. |
| `plugin_test_failed` (`missingResourceDeclarations`) | Plugin queried `pulse.tasks` / `pulse.context` / `pulse.files` without declaring matching `resources` | Add the missing permission string(s) from Section 3 to `manifest.json` `resources`. |
| `plugin_test_failed` (`unloadClean: false`) | Detached thread, unreleased `TaskGroup` / open file handle, or `LoadLibrary` cycle pinning DLL | Join threads in `deactivate`/`destroy`, call `tasks.Close()` / `commands.clear()`, close handles before `destroy` returns. |
| `PULSE_ERROR_INVALID_ARGUMENT` on `model.create` | `Stack`/`ScrollView` node given `.text`, or `TextInput`/`Select` missing accessible `.text` label | Remove `.text` from layout containers; always set `.String(&PulseUiModelNode::text, "Label")` on inputs/selects. |
| Sidecar `.exe` / `.dll` missing after install | Helper binary not bound in `manifest.json` `native.files` | Pass `--file path/to/worker.exe` to `pulse_ext.py native-manifest` (or `files=[...]` in `native_package_stage`). |
