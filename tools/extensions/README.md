# Pulse Extension Development CLI & Author Toolchain

Executable extensions in Pulse use the [C++20 Native SDK](../../sdk/native/README.md) (`PULSE_SDK_PREVIEW = 3`, `windows-x64`). Start with the [Native Author Quickstart](../../docs/extensions-sdk-quickstart.md) and [Native DLL Packages](../../docs/native-packages.md).

`pulse_ext.py` and `package_signing.py` require Python 3.8+ (standard library only, plus Windows CNG via `ctypes` for RSA signing). Historical pre-native declarative workflow notes have been archived in the repository under `docs/archive/declarative-cli-legacy.md`.

---

## Primary Native Authoring Workflow (Host-Free Offline & Optional Live Host)

Global options (`--pulse EXE`, `--offline`, `--json`) precede the subcommand. `new`, `build`, `native-manifest`, `pack`, `verify`, `test`, `install`, `enable`, `disable`, `rollback`, `gc`, and `doctor` work **100% offline without `--pulse EXE`**, and `build` / `doctor --toolchain` automatically discover `vcvars64.bat`, `cmake`, and `ninja` via `vswhere.exe` when `cl.exe` is not already on `PATH`:

```powershell
# 1. Scaffold a full C++20 plugin project with a unique package ID (offline)
python tools/extensions/pulse_ext.py --json new C:/PulseAuthor/my-rename --id com.example.rename --name "Example Rename" --template rename-panel

# 2. Configure & build x64 DLL (auto-discovers MSVC vcvars64.bat + CMake + Ninja; Release, RelWithDebInfo, or Debug)
python tools/extensions/pulse_ext.py --json build C:/PulseAuthor/my-rename --config Release

# 3. Smoke-test headlessly (<1s, zero GUI DLL imports via pulse_plugin_harness.exe + static PE64/manifest checks)
python tools/extensions/pulse_ext.py --json test C:/PulseAuthor/my-rename --smoke-commands --strict-resources

# 4. Bind DLL SHA-256 into a staged manifest, pack, and verify offline (pass --pulse C:/Pulse/pulse.exe when live host validation is desired)
python tools/extensions/pulse_ext.py --json native-manifest C:/PulseAuthor/my-rename/manifest.json --library C:/PulseAuthor/my-rename/build/com_example_rename_lower.dll --output C:/PulseAuthor/my-rename/build/manifest.json
python tools/extensions/pulse_ext.py --json pack C:/PulseAuthor/my-rename/build/manifest.json --output C:/PulseAuthor/my-rename/build/com.example.rename.pulse-ext
python tools/extensions/pulse_ext.py --json verify C:/PulseAuthor/my-rename/build/com.example.rename.pulse-ext

# 5. Install & enable in an isolated test store (offline), then optionally launch & stop an isolated test host
python tools/extensions/pulse_ext.py --json install C:/PulseAuthor/my-rename/build/com.example.rename.pulse-ext --store C:/PulseAuthor/extensions --allow-native --accept-capabilities
python tools/extensions/pulse_ext.py --json enable com.example.rename --store C:/PulseAuthor/extensions --allow-native
python tools/extensions/pulse_ext.py --pulse C:/Pulse/pulse.exe --json run --store C:/PulseAuthor/extensions --profile C:/PulseAuthor/run-1
python tools/extensions/pulse_ext.py --pulse C:/Pulse/pulse.exe --json stop-host --pid <PID> --profile C:/PulseAuthor/run-1
```

---

## Project Templates & Samples

`project_creator.py` (`pulse_ext.py new` and MCP `native_project_create`) enumerates packable full-project templates exclusively from `examples/native/catalog.json`:

- `directory-browser`: read-only `pulse.directories` + `pulse.ui.data` paged directory browser.
- `directory-transfer`: controlled file transfers to an explicitly opened destination directory (`pulse.fileops`).
- `directory-workbench`: dockable/windowed workbench using `pulse.ui.mount`, `pulse.directories`, and `pulse.fileops`.
- `file-operations`: selection copy/move/recycle-bin submission and task tracking.
- `inspector`: directory aggregation, `pulse.resources` CAS settings, and audit inspection.
- `live-data`: streaming, paged, and delta `pulse.ui.data` provider for `virtualList`/`dataTable`/`tree`.
- `reactive-panel`: background worker (`std::jthread`) + `pulse.dispatch` + C++20 `pulse/client.hpp` wrapper.
- `rename-panel`: batch rename planning and native host confirmation (`pulse.files`).

Additional SDK samples (not `new` templates):

- `examples/native/basic`: minimal ABI-only sample (`CMakeLists.txt` + `plugin.cpp`, no `manifest.json`), surfaced in MCP `native_sdk_describe.abiSamples`.
- `sdk/native/samples/archive`: multi-file SDK reference sample (`native_sdk_describe.referenceSamples`) demonstrating `native.files` sidecar binding, `pulse.commands`, `pulse.preview`, `pulse.tasks`, `pulse.staging`, `pulse.settings.pages`, and local 7z/ZIP archive writer integration (`P1.10`). Distinct from the standalone official `extensions/7zip` candidate in the main repository, which additionally integrates `pulse.dialogs` modal password/compression/checksum workflows, encrypted archives, split volumes, and tar variants against official 7-Zip 26.00.
- `sdk/native/samples/sandboxed-worker`: Stage D out-of-process reference sample (`native_sdk_describe.referenceSamples`) pairing a thin `pulse.commands` + `pulse.tasks` bridge DLL with a zero-GUI sidecar `sandboxed_worker.exe` (`native.files`) governed by a Win32 Job Object (`ActiveProcessLimit = 1`, memory cap, `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, `PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`, explicit pipe handle inheritance) and framed pipe IPC with cooperative cancellation.

`new` validates `package_id` (`1..64` ASCII `[A-Za-z0-9._-]`, rejecting Windows reserved device names and reserved `sample.*` IDs), rejects symlinks and existing destinations, renders `manifest.json` (including `"contributes": {"commands": []}`), rewrites CMake project/library target names in `CMakeLists.txt`, and writes a customized `README.md`. Legacy `init` remains available for manifest-only scaffolding compatibility.

 Static zero-load command metadata can be declared in `manifest.json` under `contributes.commands` (`id`, `title`, optional `icon` and `shortcut`) so Pulse surfaces commands in menus/palette before loading the DLL and activates the plugin on first invocation. Dynamic selection-conditioned commands continue to register through `pulse.commands` (`commands.h`) and modal forms through `pulse.dialogs` (`dialogs.h`).

---

## Host & Toolchain Diagnostics (`doctor`)

```powershell
python tools/extensions/pulse_ext.py --pulse C:/Pulse/pulse.exe --json doctor
python tools/extensions/pulse_ext.py --pulse C:/Pulse/pulse.exe --json doctor --toolchain --manifest C:/PulseAuthor/my-rename/build/manifest.json
```

- Default `doctor` checks Python compatibility and probes `--extension-api`, reporting `host.apiVersion`, `host.sdkPreview` (`3`), and `host.architecture` (`windows-x64`) while omitting all local paths and environment variables (`pathsIncluded: false`, `environmentIncluded: false`).
- `--toolchain` additionally checks `cmake` (3.25+), `ninja`, MSVC `cl.exe` targeting `x64`, and Windows SDK headers (`Windows.h`) / x64 libraries (`kernel32.lib`), reporting explicit `missingReasons` without leaking local paths.
- `--manifest PATH` checks strict UTF-8 JSON, package `native.architecture` against the host, declared `resources` against the host's advertised 15-item resource enum, and host `--extension-validate-json` acceptance.

---

## Trust Boundary & SDK Preview Check

Native plugins execute in-process with the user's OS privileges. Although the host verifies `PulsePluginSdkPreview() == PULSE_SDK_PREVIEW` (`3`) before calling `PulsePluginEntryV1`, Windows `LoadLibraryExW` maps the PE image and can execute `DllMain` and TLS callbacks **before** the host queries `PulsePluginSdkPreview`. Therefore:

- `PULSE_SDK_PREVIEW` is an **ABI compatibility check**, not a security sandbox.
- `resources` and `contributes.commands` in `manifest.json` are installation disclosures and static UI metadata, not per-call OS permission gates.
- Only install and enable `--allow-native` binaries from reviewed sources or trusted publishers.

---

## Publisher Continuity, Key Rotation & Store Garbage Collection

```powershell
# Generate a publisher key-rotation proof signed by the old private key
python tools/extensions/package_signing.py rotate --id com.example.rename --old-private-key old_priv.blob --old-public-key old_pub.blob --new-public-key new_pub.blob --output rotation.json

# Trust the new public key in the target store, then upgrade with --rotation
python tools/extensions/package_signing.py trust --store C:/PulseAuthor/signed-store --public-key new_pub.blob --fingerprint <NEW_SHA256> --require-signed
python tools/extensions/pulse_ext.py --pulse C:/Pulse/pulse.exe --json install com.example.rename-v2.pulse-ext --store C:/PulseAuthor/signed-store --signature v2.sig.json --rotation rotation.json --allow-native

# Roll back (requires --accept-capabilities if previousRevision declares extra capabilities/resources)
python tools/extensions/pulse_ext.py --pulse C:/Pulse/pulse.exe --json rollback com.example.rename --store C:/PulseAuthor/signed-store --allow-native --accept-capabilities

# Reclaim unreferenced revisions, binaries, and multi-file package directories under .registry.lock
python tools/extensions/pulse_ext.py --json gc --store C:/PulseAuthor/signed-store
```

- **Upgrade publisher continuity**: When upgrading an installed signed package, `install` inspects `versions/<activeRevision>.sig.json`:
  - Signed → unsigned upgrades are rejected with `installed_publisher_mismatch`.
  - Publisher A → Publisher B upgrades are rejected with `installed_publisher_mismatch` unless either a valid `--rotation` proof (`Pulse.Extension.Publisher.Rotation\0v1\0`, signed by A over `id` + `oldPublisher` + `newPublisher` with B trusted in `signature-policy.json`) is supplied, or `--allow-publisher-change` is passed explicitly.
  - When `--allow-publisher-change` is used **without** a valid rotation proof, `enabled` is forcibly reset to `false` so a cross-publisher replacement never silently inherits active execution consent.
- **Rollback**: Compares `previousRevision` capabilities/resources/contributed commands against `activeRevision`; any capability re-expansion requires `--accept-capabilities` (`capability_confirmation_required`), and publisher continuity is verified across revisions.
- **Garbage collection (`gc`)**: Acquires `.registry.lock` and removes files/directories under `versions/`, `binaries/`, and `packages/` not referenced by any registered package's `activeRevision` or `previousRevision`. Files currently mapped or locked by a running Pulse process are skipped and reported in `gc.skipped`. `registry.json` and `signature-policy.json` are always preserved.

---

## MCP Author Server, Agent Assets & SDK Verification Scripts

- `native_author_mcp.py`: stdio JSON-RPC MCP server (configured via `sdk/native/mcp.example.json`, with agent playbook in `sdk/native/AGENTS.md` and skill in `sdk/native/skills/pulse-plugin-author/SKILL.md`) exposing `native_sdk_describe`, `native_project_create`, `native_project_build` (`Debug` / `RelWithDebInfo` / `Release` with auto-discovered MSVC x64, `.pdb` symbols, and VS / VS Code attach hints), `native_package_stage`, `native_package_install`, `native_host_diagnose`, `native_host_run`, `native_host_stop`, `native_plugin_test` (offline manifest/PE64 + headless `pulse_plugin_harness.exe` lifecycle/command smoke checks), and `native_host_logs` (path- and environment-redacted `extension-startup-status.json`, `extension-audit`, and crash guard attribution).
- `check_docs_and_contracts.py`: verifies zero drift across `abi.h` (`PULSE_SDK_PREVIEW == 3`), `sdk/native/api-catalog.json`, `examples/native/catalog.json`, `schemas/extensions/manifest.schema.json` (15 `resources` + `contributes.commands`), agent assets (`AGENTS.md`, `SKILL.md`, `mcp.example.json`, `samples/sandboxed-worker`), and core documentation.
- `bundle_native_sdk.py` & `verify_sdk_bundle.py`: packages the standalone `Pulse-NativeSDK-<version>-windows-x64.zip` with `compatibility` metadata and verifies extraction, contract checks, offline CLI verification, and 8-template project generation outside the repository tree.
