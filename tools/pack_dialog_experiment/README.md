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
