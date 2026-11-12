---
name: pulse-plugin-author
description: Scaffold, compile (with automatic MSVC x64 discovery), smoke-test headlessly in under a second, package (.pulse-ext), and diagnose C++20 native plugins and Job-Object-isolated out-of-process worker plugins for the Pulse Windows file manager.
---

# Pulse C++20 Native Plugin Authoring Skill

Use this workflow whenever asked to create, modify, build, test, package, or debug a Pulse extension (`PULSE_SDK_PREVIEW = 3`, Windows x64 C ABI). Read [`../../AGENTS.md`](../../AGENTS.md) for the complete ABI contract and `resources` mapping table.

## 1. Choose the Right Architecture

1. **Standard In-Process Plugin (`pulse_ext.py new` / MCP `native_project_create`)**:
   - Pick the closest template from the 8 packable templates in `examples/native/catalog.json`:
     - Commands / settings / inspection: `inspector`
     - Background C++20 worker + UI state updates: `reactive-panel`
     - Batch rename with native confirmation: `rename-panel`
     - Virtualized table/list/tree streaming: `live-data` or `directory-browser`
     - Controlled file/tree operations: `file-operations`, `directory-transfer`, or `directory-workbench`
2. **Out-of-Process Sandboxed Worker (`sdk/native/samples/sandboxed-worker/`)**:
   - Choose this pattern whenever parsing untrusted files, running heavy CPU/memory workloads, or isolating third-party libraries from Pulse's GUI process.
   - Copy [`../../samples/sandboxed-worker/`](../../samples/sandboxed-worker/README.md) into your workspace, rename the `id` / CMake targets, keep the DLL as a thin `pulse.commands` + `pulse.tasks` bridge, and ship the zero-GUI helper `.exe` via `native.files` (`--file worker.exe`).

## 2. Execute the Headless Authoring Loop

Prefer the MCP tools (`native_project_create` -> `native_project_build` -> `native_plugin_test` -> `native_package_stage` -> `native_package_install`) when the `pulse-native-author` MCP server is connected (see [`../../mcp.example.json`](../../mcp.example.json)), or run `tools/extensions/pulse_ext.py` directly. Neither requires a pre-configured Visual Studio prompt or a running `pulse.exe` GUI:

1. **Scaffold**:
   ```powershell
   python tools/extensions/pulse_ext.py --json new <project_dir> --id <reverse.dns.id> --name "<Display Name>" --template <template>
   ```
2. **Edit Manifest & C++20 Source**:
   - Keep `PULSE_PLUGIN_SDK_PREVIEW()` at file scope and `PulsePluginEntryV1` exported with `extern "C" __declspec(dllexport)`.
   - Declare every static menu/palette command in `manifest.json` under `contributes.commands` and register matching runtime handlers via `pulse.commands` in `PulsePluginEntryV1`.
   - Declare exact `resources` in `manifest.json` matching every host interface your plugin queries (e.g., `["context.read", "operations.own"]` for selection commands + `pulse.tasks`).
3. **Compile (Auto-MSVC Discovery)**:
   ```powershell
   python tools/extensions/pulse_ext.py --json build <project_dir> --config Release
   ```
4. **Headless Smoke & Contract Test (<1s)**:
   ```powershell
   python tools/extensions/pulse_ext.py --json test <project_dir> --smoke-commands --strict-resources
   python tools/extensions/pulse_ext.py --json test <project_dir> --omit-experimental-ui
   ```
   - Verify `valid: true`, `staticBinary.hasEntryV1: true`, `staticBinary.hasSdkPreview: true`, and (when `pulse_plugin_harness.exe` is present) `unloadClean: true` with zero `missingResourceDeclarations` or `unregisteredStaticCommands`.
   - Pass `--selection <fixture_file>` and `--invoke-command <cmd_id>` to exercise real command + worker execution against isolated test fixtures.
5. **Stage, Pack & Verify `.pulse-ext`**:
   ```powershell
   python tools/extensions/pulse_ext.py --json native-manifest <project_dir>/manifest.json --library <dll_path> [--file <sidecar_path>] --output <stage_dir>/manifest.json
   python tools/extensions/pulse_ext.py --json pack <stage_dir>/manifest.json --output <stage_dir>/<id>.pulse-ext
   python tools/extensions/pulse_ext.py --json verify <stage_dir>/<id>.pulse-ext
   ```
6. **Isolated Install & Host Diagnostics (Optional)**:
   - Always test installation against an isolated `--store <workspace>/test-store`, **never** `%LOCALAPPDATA%\Pulse`.
   - When an isolated host is launched (`native_host_run`), inspect `extension-startup-status.json`, `extension-audit`, and crash guard attribution via MCP `native_host_logs`, then close cleanly via `native_host_stop` before rebuilding.

## 3. Pre-Completion Verification Checklist

- [ ] Zero exceptions can cross any `PULSE_CALL` function pointer.
- [ ] No `PulseStringView` pointer is retained after its callback returns; async tasks copy inputs into `std::string`.
- [ ] No blocking I/O or process wait runs on the owner UI thread.
- [ ] Plugin gracefully handles `PULSE_ERROR_UNAVAILABLE` if any `experimental` `pulse.ui.*` interface is unavailable.
- [ ] `pulse_ext.py --json test ... --strict-resources` and `pulse_ext.py --json verify <pkg>.pulse-ext` both succeed cleanly.
