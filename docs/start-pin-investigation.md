# Start pin invocation investigation — 2026-10-29

> Historical investigation recorded on 2026-10-29. Statements about implementation status below describe that investigation, not the current product. Later Start pin bridge and menu implementation and tests supersede those status statements; preserve the observations as diagnostic history.

## Required behavior

Clicking Pin to Start for a normal desktop folder in Pulse must directly pin that
folder, without opening Explorer or creating a different Pulse-specific tile.
The user explicitly rejected those alternative interactions. This behavior is
not implemented or verified as fixed.

## Reproduced observations

- Native `IContextMenu` obtained through `IShellFolder::GetUIObjectOf` exposes
  `PinToStartScreen`, but its invocation from the independent diagnostic process
  returns `0x80070005` (E_ACCESSDENIED). The folder remains unpinned.
- The same result occurs outside the restricted command environment.
- Adding `CMIC_MASK_NOASYNC` did not fix the user's reproduction; that attempted
  production change was reverted.
- Calling Explorer's `IFolderView2::InvokeVerbOnSelection` returns S_OK, but an
  independent query still exposes `PinToStartScreen`. S_OK is not proof of pinning.
  Both the lowercase and system-provided mixed-case verb were checked.
- Obtaining Explorer's selection `IContextMenu` remotely and invoking the menu ID
  also returns E_ACCESSDENIED. Remote GetCommandString returned `0x800706f4`, so the
  diagnostic matched the localized menu title to identify the ID. This diagnostic
  title matching is not proposed as production code.
- Clicking the actual Explorer context menu on the isolated TestPin folder did
  change the independently queried verb to `UnpinFromStartScreen`.
- The corresponding actual native unpin was performed, and the independent query
  returned `PinToStartScreen` again. The test pin was removed.

The actual pin/unpin fixture was:
`C:\Users\SS\Desktop\pulse\PulseExplorerPinProbe-29048\TestPin`.
No user's pre-existing folder was pinned or unpinned by these tests.

## System implementation evidence

Read-only inspection of this machine's `C:\Windows\System32\appresolver.dll`:

- File version: `10.0.22621.4745 (WinBuild.160101.0800)`.
- SHA-256: `0271417DCFFAA4AF948DC9E0D106671508E96CC3A0E39DF19FEA7435D12698A3`.
- Symbol names resolved using Microsoft's public symbol server and the installed
  Windows SDK dbghelp.dll. Disassembly produced using MSVC dumpbin.
- Preferred image base `0x180000000`; addresses below apply only to this image.
- `StartPinUnpinContextMenu::InvokeCommand` at RVA `0x3DCE0` calls
  `ValidateCallerIdentity` (RVA `0x3FE48`) before `InvokeCommandImpl`. A failed
  identity check exits before the invocation body.
- The identity check obtains the calling process handle, queries its full image
  name, expands the UTF-16 string `%SystemRoot%\\explorer.exe` at RVA `0x89700`,
  and compares the paths using `CompareStringOrdinal` with ignoreCase = TRUE.
- If the Explorer path comparison fails, it checks `IsProcessAppContainer` and
  `CheckCapabilityFromProcessHandle`. The capability name read from RVA `0x8D648`
  is `shellExperience`.
- If neither path qualifies, RVA `0x3FF76` loads `0x80070005` as the return value.

This explains why displaying the menu works while the ordinary Pulse host cannot
execute the command, and why changing synchronous flags does not solve it. Merely
adding ordinary package identity would not grant the system shell capability.
No system DLL was patched, process identity spoofed, or code injected into Explorer.

Local diagnostic files are under `bench_data/start_pin_live/`: `symbol_probe.cpp`,
`symbols.txt`, `appresolver_disasm.txt`, `imports.txt`, and the two Explorer probes.
These probes are diagnostic artifacts, not passing product regression tests.

## Relevant upstream material

### Isolated Explorer extension experiment (not a product fix)

`bench_data/start_pin_extension/` contains a temporary COM context-menu and
DelegateExecute extension restricted to a unique diagnostic file association.
Its worker only targets `NativeFolder` inside that diagnostic directory.
Local COM activation and local menu construction succeeded. Explorer's automation
menu did not discover either the dynamic handler or the static registered verb,
including with fresh associations and a fresh CLSID. The parsed item path was
verified. No extension worker ran and no pin success was observed.

Explicit Explorer ShellExecute produced a no-association dialog; it was closed.
The command returned S_FALSE, which is not evidence of successful execution.
Every completed client run reported successful removal of its three temporary
HKCU registration roots. The test processes and Explorer ran under the same SS
account. These facts do not yet explain Explorer's different association view.

A machine-level registration comparison was prepared but rejected by automatic
approval review before execution. Explicit user approval has been requested.
The diagnostic now defaults to HKCU; HKLM requires an explicit `--machine` switch.
No production source change has been made for this experimental approach.

- Directory Opus developers report the same Explorer-only restriction:
  https://resource.dopus.com/t/right-click-context-menu-to-pin-to-start-and-pin-to-taskbar/55991
- Microsoft's supported desktop secondary-tile API requires package identity and
  a user confirmation dialog, and creates an app-owned secondary tile. It does not
  meet the requested unchanged native-folder interaction:
  https://learn.microsoft.com/en-us/windows/apps/design/shell/tiles-and-notifications/secondary-tiles-desktop-pinning

## Build and current changes

The diagnostic host was built successfully. The menu lifetime audit executable
passed, but it does not test Start pinning and is not evidence of this bug being
fixed. A later `pulse` build temporarily failed because an unrelated concurrently
added target referenced a missing file; after that file appeared, the targeted
build completed with `ninja: no work to do`.

The temporary production logging of handler, item IDs and InvokeCommand HRESULT
was removed after diagnosis. No functional production fix is being delivered.
Diagnostic sources and evidence remain available under the paths above.
After closing the two isolated test instances started during this investigation,
`cmake --build build --target pulse_shell` successfully rebuilt the host without
the temporary logging. The initial cleanup build could not replace the running
executable (LNK1104); the final link succeeded once those test processes exited.

The system DLL hash was rechecked and still matches the identity-check evidence
above. The requested direct native behavior is blocked by that unchanged caller
restriction; the task is not complete.

## Outstanding

The original one-click goal remains unmet. A supported native-folder pin entry
point callable by an ordinary Pulse process has not been found. Explorer handoff
and app-owned tiles were proposed and rejected by the user. An alternative would
need to satisfy the same direct native-folder behavior and be verified by actual
pin and unpin state changes, not HRESULT alone.
