# Versioned dialog experiment bundle

This tool packages the explicitly selected R8 experiment, **not an installer or
capability certification**. It does not change provider gates, registry entries,
profiles, active processes, or loaded modules. `VERIFIED.json` certifies only the
listed files and statically inventoried dependencies at verification time. It is
not a signature, proof of native validation parity, or approval to enable a runtime.

## Commands

Python 3.10+ standard library only; no dumpbin/pefile, DLL load or executable run:

```powershell
python -B tools/pack_dialog_experiment/test_pack.py
python -B tools/pack_dialog_experiment/pack.py plan --build C:/absolute/pulse/build --version R8-experiment-1 --arch x64
python -B tools/pack_dialog_experiment/pack.py pack --build C:/absolute/pulse/build --version R8-experiment-1 --arch x64 --output C:/absolute/experiments/R8-experiment-1-x64
python -B tools/pack_dialog_experiment/pack.py verify --directory C:/absolute/experiments/R8-experiment-1-x64
```

The output parent must already exist. The output directory itself must **not**
exist, including an empty directory. Source and destination symlink/reparse paths
are rejected. Output may not be inside build. Failure leaves its incomplete new
directory for diagnosis, never deletes it, and never produces a successful
verification marker before verification. A later attempt needs a different new
directory. There is no overwrite, cleanup, deployment or hot-upgrade switch.

`plan` reads only; it reports unresolved dependencies together where possible.
`pack` checks before copying, copies only the closure and listed licenses with
exclusive file creation, hashes each copy against the plan, writes the manifest,
recomputes PE imports and hashes from the destination, then writes and rechecks
VERIFIED.json. `verify` rejects extra files, missing files, malformed/unknown
contracts, changed hashes/imports, wrong architecture and unsafe relative paths.

## Closure and scope

Required entry files are PulseDialogLauncher.exe, pulse.exe,
PulseFileDialogRuntimeUiProof64R8.dll (32R8 for x86), PulsePickerUiProof.dll,
lumatext.dll, and pulse_shell.exe. This first package requires every PE to match
the selected architecture. The profile's ability to use a different-bitness
out-of-process host is deliberately not used to mix this in-process bundle.

Explicit dynamic dependencies, with code references in `DYNAMIC`:

- Pulse.Preview.exe: module-relative thumbnail worker.
- pdfium.dll: dynamically loaded PDF renderer used by preview.
- pulse_elevated.exe: explicit elevated file-operation helper. Including it does
  not grant elevation or prove its invocation from every third-party process.

Normal and delay import DLL names are recursively resolved from **build's direct
children only**, with case-collision rejection. Unknown/missing names fail; PATH,
CWD, application install folders and System32 are never dependency search sources.
Windows DLLs and API-set contract names use an explicit system-requirement policy,
are recorded but not copied. A local file shadowing a system dependency fails.
This does not prove availability of every Windows export on an older OS.
The initial scan additionally identified documented system dependencies
[Rstrtmgr.dll](https://learn.microsoft.com/en-us/windows/win32/api/restartmanager/nf-restartmanager-rmendsession)
and [XmlLite.dll](https://learn.microsoft.com/en-us/archive/msdn-magazine/2007/april/xmllite-a-small-and-fast-xml-parser-for-native-c).

The PE reader adapts the repository's tools/audit_win81_imports.py RVA/import
approach, adding PE32, machine/magic pairing, bounded headers/sections/import
terminators/names and delay VA support. It inventories module names, not exported
symbols, transitive system DLLs, COM registrations or arbitrary LoadLibrary code.
The explicit helper list is a reviewed minimum picker/preview/operation closure;
it does not certify every standalone pulse.exe feature (AI, index service,
document editing, integration installer) in this experiment bundle. New dynamic
edges need source review and a focused test; no wildcard build/assets copy.

Resources in picker_ui.rc are embedded. Installed/system fonts and normal Windows
components remain OS prerequisites. Preserve the user's required Pulse appearance;
a static package pass never substitutes for an actual visual/input check.

Licenses include build/licenses/LumaText and build/licenses/PDFium, plus the
repository's Pulse, Detours, ib-pinyin, md4c licenses and SQLite/LZMA repository
notices. Missing groups/notices fail. This inventories the existing distribution
notices; it is not an independent legal audit of all third-party code.

## Version/lifecycle contract

Each new version/architecture gets a new directory. Keep old packages intact
while any client could hold their pinned runtime/UI modules. A new managed
profile can point at the new version after package verification; do not edit an
existing active directory or unload modules to update it. Legacy v1 profiles bind launcher/host/runtime identities only. The managed
launcher `--bundle` path and v2 profiles additionally verify the complete manifest,
its bound digest and dependency closure, retaining file handles through launch and
initialization. The runtime loader itself does not independently repeat the whole
bundle verification. Explicit Python `verify` remains a packaging check; neither
verification path grants capability certification.
Hashes and the marker are unauthenticated and can be changed by the same writer;
use controlled directories. Concurrent hostile modification of source/output by
the same user is not prevented by this Python tool; deploy from quiescent builds.

Still required after root performs real packaging: verify actual packaged imports,
load with a foreign caller directory/CWD, Open/Save/cancel/standard events,
preview/helper availability, DPI/text visibility, and old/new version coexistence.
No script operation in this directory attaches to or launches a third-party app.

## Explicit x64 + x86 deployment

`multi_arch.py` keeps **two complete, architecture-pure schema-1 bundles** and a
bounded `multiarch.json` index that binds both child manifest byte hashes. It
does not remove the single-bundle mixed-PE rejection. Each child includes its
own architecture-matched host, runtime, UI, automatic COM DLL, native control,
dependencies and licenses. Both builds must already contain the full closure;
the packer never silently borrows a missing x86 binary from the x64 build.

```powershell
python -B tools/pack_dialog_experiment/multi_arch.py pack --build-x64 C:/absolute/pulse/build --build-x86 C:/absolute/pulse/build-bridge-x86 --version R8-dual-1 --output C:/absolute/experiments/R8-dual-1
python -B tools/pack_dialog_experiment/multi_arch.py verify --directory C:/absolute/experiments/R8-dual-1
python -B tools/pack_dialog_experiment/multi_arch.py status --directory C:/absolute/experiments/R8-dual-1
python -B tools/pack_dialog_experiment/multi_arch.py enable --directory C:/absolute/experiments/R8-dual-1
python -B tools/pack_dialog_experiment/multi_arch.py disable --directory C:/absolute/experiments/R8-dual-1
# Change just one view (the integrity check still covers BOTH bundles):
python -B tools/pack_dialog_experiment/multi_arch.py enable --directory C:/absolute/experiments/R8-dual-1 --arch x86
python -B -m unittest discover -s tools/pack_dialog_experiment -p test_multi_arch.py -q
```

Unlike the pure-file `pack.py`, explicit `status`/`enable`/`disable` execute each
verified native control. Native x64 and x86 controls use the 64-bit and 32-bit
HKCU COM registry views respectively. A 64-bit Windows host is required to run
both controls. The native controls must provide `--status-json`,
`--enable-json ABSOLUTE_BUNDLE` and `--disable-for-json ABSOLUTE_BUNDLE`.
Their stdout is one UTF-8 JSON object with `error`, `enabled`, `conflict`,
`status_error`, and `dll_path`. Their exit code and JSON error must agree.
Older controls without this protocol fail before the tool reports a mutation
as successful. Native enable checks for another bundle under its registration
mutation lock; native disable is scoped to the expected DLL path under the same
lock. The tool itself never writes the registry or invokes `reg.exe`.

Both closures are reverified before every native launch, including a read-only
status command and a selected-architecture command. All selected views pass a
status/ownership preflight before any change. Conflicts, unreadable status or
another bundle's recorded path prevent changes. A successful enable/disable
reports both view states. A later-view failure attempts to restore the prior
enabled/disabled state of only the views attempted by this transaction, in
reverse order, including the failing view's partial journal. A preexisting
enabled view skipped by enable is not disabled by rollback. Results distinguish
`ok`, `failed`, `rolled-back` and `rollback-failed`; a failed request always exits
1, including a successful rollback. JSON records every native operation and
rollback outcome. Recovery never imports a registry snapshot over external
changes. The process-wide named orchestration mutex serializes these tools;
native per-call locks still protect individual mutations. This is transactional
recovery on ordinary errors, not a crash-persistent distributed transaction or
an atomic switch against a separate concurrent legacy control process.

The wrapper is an executable development/deployment path, **not installer
integration**. Production `dialog-current.json`, the stable launcher, setup
upgrade/uninstall, and the application settings UI continue to select their
existing single-architecture bundle. They do not consume `multiarch.json`.
Keep both new children immutable while callers can hold modules. Disabling
registration stops future interception; it does not unload an existing caller's
modules or delete deployment files. Use per-architecture real caller/UI checks
before drawing conclusions about dialog behavior. The synthetic test suite
checks packaging and orchestration using fake native responses; it is not a
claim that either real registry view or a third-party dialog was exercised.

## Thin x86 companion for installed x64 packages (schema 3)

`pack_installer.py --dialog-build-x86` now packages a thin x86 companion and
keeps the complete x64 application in its existing schema-1 addressed bundle.
The companion's roots are `PulseDialogLauncher.exe`, `PulseDialogAuto32.dll`,
`PulseDialogAutoControl.exe`, and `PulseFileDialogRuntimeUiProof32R8.dll`, plus
their real normal/delay PE dependencies and Pulse/Detours licenses. It does not
collect the full x86 `DEFAULT_LAYOUT`, PDFium, LumaText, or preview worker.
Use freshly built thin-capable native components; schema-1-era binaries are
not made thin-capable by repackaging them. Synthetic test PEs are not runnable.

Schema 3 is x86-only. It retains the schema-1 top-level keys, sets
`dynamic_helpers` to `{}`, and adds exactly:

```json
"host_bundle": {"manifest_sha256": "<64 lowercase hexadecimal characters>"}
```

The host is resolved only as the companion directory's sibling named by that
hash. It must have a matching manifest hash and verification marker, complete
schema-1 x64 PE closure and automatic components, and the identical version.
Thin-host chains are rejected before recursion. Installed payload verification
also requires the host hash to equal `dialog-current.json`'s primary hash.
The primary pointer still requires schema 1; older full schema-1 x86 companion
payloads remain valid. `multi_arch.py` continues to produce its original two
full schema-1 experimental bundles.

Programmatic `plan`, `package`, and `package_addressed` accept keyword arguments
`thin=True, host_digest=<hash>`. Planning checks syntax and the x86 PE closure;
packaging/verifying additionally require the complete sibling host. Standalone
`pack.py plan/pack` expose `--thin --host-digest <hash>`. For `pack`, place the
new output directory alongside the already addressed x64 host, never elsewhere.

Focused synthetic validation (does not execute or install binaries):

```powershell
python -m unittest discover -s tools/pack_installer -p test_pack_installer_thin.py
python -m unittest discover -s tools/pack_installer -p test_pack_installer_dual.py
python -m unittest discover -s tools/pack_dialog_experiment -p test_pack.py
```
