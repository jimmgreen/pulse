# Archived Reference: Legacy Declarative Extension CLI Workflows

> [!WARNING]
> **Historical Archive — Not Current Guidance.**
> Executable extensions in Pulse now use only the **C++20 Native SDK** (`PULSE_SDK_PREVIEW = 3`, `windows-x64`). Legacy declarative templates (such as `dock-panel`, `selection-sort-panel`, `grouped-panel`, `workspace-layout`, `workspace-control-panel`), `isolatedDevReload` (`pulse_ext.py dev --profile`), and the historical TypeScript/preview bundle (`bundle_sdk.py`, `Pulse.ExtensionPreview.exe`) are archived below strictly as historical reference.
>
> For current plugin development, see:
> - [Native Author Quickstart](../extensions-sdk-quickstart.md)
> - [Native DLL Packages](../native-packages.md)
> - [Pulse Extension Development CLI](../../tools/extensions/README.md)
> - [Pulse Native SDK Reference](../../sdk/native/README.md)

---

## Historical Declarative Discovery & Templates

Earlier pre-native previews experimented with declarative JSON command/panel templates validated against host capability discovery (`inspect-sdk` and `compare-sdk`):

- `selection-sort-panel`: demonstrated `selection.selectAll`, `selection.invert`, `selection.clear`, and `view.setSort` gated by `selectionAndSorting`.
- `transfer-panel`: demonstrated no-argument dual-pane controlled transfers (`controlledFileTransfers`, `context.transferStatus`).
- `grouped-panel` / `workspace-layout` / `workspace-control-panel`: demonstrated declarative command groups, `workspace.setLayout`, and `workspaceNavigation` (`tabs.*`, `panes.focus`).
- `dock-panel` (removed from `examples/native/catalog.json`): historical declarative sidebar layout sample superseded by native `pulse.ui.mount` (`examples/native/directory-workbench`).

## Historical `dev --profile` (`isolatedDevReload`)

`pulse_ext.py dev PATH --profile NEW_DIRECTORY` was designed for hosts advertising `isolatedDevReload` for declarative JSON manifests (`--extension-dev`). Native C++ DLLs are never hot-replaced in-process; native plugin authors instead use isolated store runs (`pulse_ext.py run --store STORE --profile PROFILE`) paired with cooperative shutdown (`pulse_ext.py stop-host --pid PID --profile PROFILE`).

## Historical Version 1 Declarative Archive & Maintainer Preview Bundle

- Declarative-only archives (`formatVersion: 1`) contained `manifest.json` and `package.json` without a native PE payload. Executable plugins now use `formatVersion: 2` `.pulse-ext` packages binding `manifest.json`, `package.json`, the x64 entry DLL, and optional `native.files` sidecars.
- Maintainer scripts such as `bundle_sdk.py` and `typecheck_sdk.py` applied to the historical declarative metadata preview bundle rather than the C++ Native SDK (`bundle_native_sdk.py` / `verify_sdk_bundle.py`).
