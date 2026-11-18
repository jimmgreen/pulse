# Stage 6 focused evidence — 2026-11-08

Existing support was reused: cmake/PulseDialogAuto.cmake already builds PulseDialogAuto32.dll with x86 exports; scripts/vcvars.bat x86 selects Hostx64/x86. No production code or registration was changed.

## Actual commands and results

From C:/Users/SS/Desktop/pulse:

- cmd /c "call scripts\vcvars.bat x86 >nul && cmake --build build-bridge-x86 --target pulse_legacy_adapter_test pulse_legacy_ansi_adapter_test pulse_legacy_ansi_utf8_test pulse_dialog_auto": exit 0, includes PulseDialogAuto32.dll.
- cmd /c "call scripts\vcvars.bat x86 >nul && cmake --build build-bridge-x86 --target pulse_picker_host_test": exit 0.
- build-bridge-x86/pulse_legacy_adapter_test.exe: exit 0, 25 PASS, 0 FAIL; injected ABI/model test, not real UI.
- build-bridge-x86/pulse_legacy_ansi_adapter_test.exe: exit 0, Checks: 44, failures: 0.
- build-bridge-x86/pulse_legacy_ansi_utf8_test.exe: exit 0, Checks: 53, failures: 0.
- build-bridge-x86/pulse_picker_host_test.exe C:/Users/SS/Desktop/pulse/build/pulse.exe --legacy: exit 0, Checks: 35, failures: 0. Real x86 caller to x64 Pulse UI, Open/Save/Cancel, owner/modality/output-buffer contract, native=0.
- Same command with --legacy-ansi: exit 0, Checks: 38, failures: 0. Real ANSI caller to Pulse UI, Open/Save/Cancel.
- python -m unittest discover -s tools/pack_dialog_experiment -p test_pack.py -k automatic_architectures: exit 0, Ran 1 test, OK. Existing synthetic x86/x64 bundle architecture and wrong-architecture checks. Not a release package test.
- build/pulse_dialog_admin_trust_test.exe: exit 0; existing binary, ACL checks including user-writable binary rejected error=5.
- build/PulseDialogLauncher.exe --admin-check C:/Users/SS/Desktop/pulse/build: exit 2, admin_tree_valid=0 error=5 is_elevated=0.

Raw logs: C:/Users/SS/Desktop/pulse/bench_data/stage6-20261108/*.log. Existing x64 host used, timestamp 2026-11-08 01:39:51; not rebuilt by this audit.

## Remaining coverage boundaries

The real legacy tests call the explicit LegacyFileDialogAdapter with an exact-fixture gate; they do not establish global interception of arbitrary GetOpenFileName imports or third-party application compatibility. No global registration was touched.

Elevated COM activation cannot rely on the HKCU override. Existing source offers an explicit PulseDialogLauncher --admin-session --bundle path, requires an already elevated caller, protected bundle/control tree, and matching integrity; automatic server also checks protected-tree trust for elevated callers. Current shell is not elevated and workspace tree fails that trust check. An end-to-end elevated run needs an administrator-protected verified bundle and explicit elevated launch; it was not performed, and ACL-model checks do not replace it.

No fixes were necessary in the tested x86/legacy components. Stage 6 is not fully complete: elevated live launch, packaged cross-architecture deployment and actual application GetOpenFileName coverage remain unverified.
