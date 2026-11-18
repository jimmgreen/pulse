# Managed launch profile (experimental)

`PulseDialogLauncher --save-profile <absolute file>` accepts the existing
`--host`, `--dll`, `--cwd`, `--control-root`, and `-- <absolute target> [arguments]`
syntax. Saving does not start a process. `--check-profile <absolute file>` only
validates. `--profile <absolute file>` validates again and uses the existing
controlled new-process launcher. Extra override arguments are rejected.

The bounded version-1 binary format uses explicit little-endian UTF-16 and
preserves argument boundaries, including empty arguments. Unknown versions,
trailing data, malformed UTF-16, relative/UNC/traversal paths and excessive
lengths are rejected. Save uses CREATE_NEW; an existing profile is not replaced.

The component directory must contain the actual launcher, pulse.exe and the
explicit runtime DLL. Their volume/file identities, sizes and modification times
are captured and rechecked. This is **not cryptographic version verification**,
and does not bind the separately loaded PulsePickerUiProof.dll or its resources.
It does not hold image locks through process creation. A future published bundle
requires its own complete integrity/deployment contract. Target applications may
update in place: their current absolute path and PE architecture are revalidated,
not pinned to the previously captured file identity. Launcher/runtime/target must
have the same architecture; the separate Pulse host may have either supported
architecture. Profile input is local-drive, existing, non-reparse paths only.

No UI capability is enabled by saving a profile. Explicitly selecting an existing
proof runtime retains that experiment's existing gates. There is no existing
process attachment, application-name allowlist, automatic process enumeration,
registry change, runtime unloading or browser-specific configuration.

Status reports process liveness, **historical** ready-handshake evidence, and the
local disable marker separately. `routing_status=not_queried` means that current
runtime policy and ancestor-session state have not been queried by the launcher.
The runtime continues to enforce ancestor disable markers itself. Exit code 0
from status certifies only the stated local checks and historical handshake, not
that a particular dialog will use Pulse. A disabled or exited session returns 3.

## Focused verification (root runs; no browser)

Build `pulse_dialog_launcher` and `pulse_managed_launch_profile_test`. The latter
uses only a uniquely created bench_data directory and never executes its copied
PE test files. Its default invocation checks the codec, exact component binding,
allowed target replacement and real disable marker states.

From the repository's PowerShell directory, with the ordinary runtime already
built in the same component directory:

```powershell
$repo = (Get-Location).Path
$bundle = Join-Path $repo 'build'
$fixture = Join-Path $repo ('bench_data\managed-launch-live-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $fixture | Out-Null
$profile = Join-Path $fixture 'launch.profile'
$launcher = Join-Path $bundle 'PulseDialogLauncher.exe'
& $launcher --save-profile $profile --host (Join-Path $bundle 'pulse.exe') --dll (Join-Path $bundle 'PulseFileDialogRuntime64.dll') --cwd $fixture --control-root $fixture -- (Join-Path $bundle 'pulse_managed_launch_profile_test.exe') --launch-fixture (Join-Path $fixture 'launch.tsv') 'two words'
& $launcher --check-profile $profile
& $launcher --profile $profile
# Repeat the same profile. Each target writes launch-<pid>-<tick>.tsv.
& $launcher --profile $profile
Get-Content (Join-Path $fixture 'launch-*.tsv')
```

Use the matching 32-bit bundle/runtime filename for x86. The fixture verifies the
exact argument, current PID/creation time/nonce-bound successful ready response,
writes its own report and stays alive for three seconds before returning 0.
Expected launcher output first distinguishes `process_started=1` from
`runtime_handshake=0`, then reports historical handshake data. These runs do not
certify actual picker display. Missing handshake / single-instance relay remains
an explicit not-connected result; the launcher leaves resumed applications alive.
Capture the owned session path while the fixture remains alive to exercise
`--disable <session>` followed by `--status <session>` (local_disabled=1, exit 3);
after exit, status must reject the exited target. The normal unit test separately
checks actual marker creation/removal without timing dependence.
For deterministic status/disable inspection, save a separate profile with the
additional target argument `--wait-release` after `'two words'`. That fixture
remains alive until the caller creates `release-<reported pid>.signal` in its
owned fixture directory, with a 20-second maximum. Missing release returns 6,
not success. While it waits, run status, disable and status against the launcher
printed session path; retain the child process handle, create the release file,
wait for that process to exit, then repeat status. This avoids relying on the
normal three-second hold for the inspection assertions. All files remain in the
unique bench_data fixture for diagnosis.

When inspecting the `--wait-release` fixture from PowerShell, start the launcher
with `Start-Process -PassThru -WindowStyle Hidden` and redirected output files;
call `WaitForExit(12000)` on that launcher process only. Do not use `-Wait` or a
synchronous redirected native pipeline for this inspection: the owned child can
keep the output stream open until its release timeout. The focused runner is
`bench_data/verify_managed_launch.ps1`; it verifies both child exit codes and the
post-exit rejection. The 20-second fixture deadline remains unchanged.


## Complete experimental bundles (profile v2)

The new `--bundle <absolute-directory>` mode verifies schema/contract 1, the
manifest marker, every listed file hash and size, PE architecture and ordinary /
delay imports, required dynamic helpers/licenses, and absence of unlisted files.
It uses the C++ verifier; Python is not needed to launch. This is integrity and
version binding, not a publisher signature or additional dialog compatibility.
Current C++ package entry names must be ASCII; parent/root paths may be Unicode.
The launcher must itself be the verified package's `PulseDialogLauncher.exe`.
Do not add logs, profiles or session control files inside a verified package.

```
<new-package>\PulseDialogLauncher.exe --bundle <absolute-new-package> --cwd <absolute-working-directory> --control-root <absolute-control-directory> -- <absolute-target.exe> <args>
```

Add `--save-profile <absolute-new-profile>` before `--` to save without launching.
That writes profile v2, binding the manifest SHA256 as well as the existing
host/runtime/launcher identities. `--check-profile` and `--profile` revalidate all
files every time; the latter holds file handles through launch/handshake and
rechecks the directory immediately before process creation. `--host`/`--dll`
overrides are rejected together with `--bundle`. Old explicit commands and v1
profiles retain their old three-component checks; they do not silently become
whole-package verification. Application updates at the configured target path
are still allowed subject to architecture/path checks. A changed component
package needs an explicitly selected new package and newly saved profile.

The lease denies write/delete sharing for known files. It is not protection
against an administrator, hostile preexisting mappings, or changes after the
launcher releases its lease. Directory enumeration is a bounded observation,
not an atomic directory freeze. Unknown files are rejected at verification and
again before launch. Do not update an in-use package; build a new version folder.
Runtime capability and unsupported-contract fallback gates are unchanged.

### Focused v2 launch fixture

Build the new launcher, bundle verifier test and existing managed-profile test.
Package that new launcher and its current components into a **new** R10 directory;
do not overwrite R9. The following placeholders must be absolute paths. `fixture`
is a newly created direct child `bench_data/managed-launch-<unique>` beside the
build directory containing the self-test executable. `control` is outside the
package. Create both directories before saving:

```
<r10>\PulseDialogLauncher.exe --bundle <r10> --cwd <fixture> --control-root <control> --save-profile <fixture>\profile-v2.bin -- <build>\pulse_managed_launch_profile_test.exe --launch-fixture <fixture>\report.tsv "two words" --wait-release
<r10>\PulseDialogLauncher.exe --check-profile <fixture>\profile-v2.bin
<r10>\PulseDialogLauncher.exe --profile <fixture>\profile-v2.bin
```

Use the existing asynchronous Start-Process inspection procedure above to avoid
inherited output-pipe waits. Check handshake and exact argv report; status, disable,
then status must report disabled; create `release-<pid>.signal` within the unchanged
20-second fixture budget, verify child exit 0 and stale-status rejection.

The standalone `pulse_dialog_bundle_test.exe` creates synthetic, never-executed
PE/manifest files only under its unique `bench_data/dialog-bundle-test-*` folder.
`pulse_dialog_bundle_test.exe --verify <r10>` only reads the supplied complete
package and checks the retained directory; it does not launch or load components.
