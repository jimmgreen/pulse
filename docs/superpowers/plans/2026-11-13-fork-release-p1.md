# Fork release and P1 fixes implementation plan

> For agentic workers: use superpowers:executing-plans for coordination and scoped parallel workers for the three independent fixes. Follow test-driven-development and verification-before-completion.

**Goal:** Configure ClauxQz4/pulse for independently signed updates, fix the three P1 issues from the approved review, and update the user's installed Windows x64 application.

**Architecture:** Keep the existing Win32/C++20 application, packaging and P-256 update signature format. Fix each issue in its owning module. Keep the private signing key outside Git with restricted access; commit only the public key. Preserve installed user settings and application data.

**Tech stack:** C++20, MSVC v143, CMake/Ninja, PowerShell 7, Python, GitHub Actions.

**Spec:** User-approved review findings in this chat: unsafe uninstall junction traversal; delayed move deletion of a replaced/modified source; AI/MCP authorization checked against a different target than the one executed. User additionally requested fork publication/update configuration, signing setup, and installation of the resulting fixes.

## Constraints and review focus

- Only the three P1 fixes and release/update configuration; P2 issues remain out of scope unless inseparable from a P1 correction.
- Test fixtures must remain isolated from real user files, settings and installed data.
- Cleanup must not follow root or child directory reparse points, including replacement races where practical.
- A move may delete only the source object/version actually copied; mismatch preserves the changed source and reports the incomplete move.
- AI execution must bind or revalidate the resolved target and applicable permission/confirmation before external effects.
- New public key must match the private key and verify the generated update manifest; no private bytes in code, logs, commits or build artifacts.
- Release scripts must target the fork. Third-party assets pinned to upstream are not rewritten indiscriminately.
- Run targeted regression tests and packaged-payload checks. No unrelated complete historical test suite.

## Task 1: Uninstall cleanup

- [x] Add isolated junction regressions for both root and child links; demonstrate failure against the old helper.
- [x] Fix `src/setup/setup_common.cpp` and directly dependent cleanup code to remove links without traversing their targets.
- [x] Verify external sentinels survive, owned ordinary files are removed, and existing setup tests pass.

## Task 2: Delayed move deletion

- [x] Add a deterministic regression in the existing operations test infrastructure: replace or modify a source between copy completion and move commit.
- [x] Fix `src/ops` source identity/version protection and deletion, without changing unrelated undo behavior.
- [x] Verify ordinary move success, cancellation with pending moves, and source-preservation failure reporting.

## Task 3: AI/MCP execution authorization

- [x] Add a safe regression covering selection/navigation changes before `nav.open`; use harmless test targets and no real external launch.
- [x] Fix `src/app/ai_agent_files.cpp`, `ai_agent_mcp.cpp` and related owned policy code so authorization and execution refer to the same target.
- [x] Verify disabled external permissions cannot be bypassed and allowed operations retain correct confirmation behavior.

## Task 4: Fork release identity and signing

- [x] Audit CMake, local build scripts and release workflow for update/publish repository settings.
- [x] Set the fork repository and next local release version; preserve pinned dependency sources.
- [x] Generate an ECDSA P-256 key outside Git with private ACLs and update the committed public key.
- [ ] Replace the fork's `PULSE_UPDATE_PRIVATE_KEY` with the current key after GitHub account verification; deferred by the user while local build/install proceeds.
- [x] Add or update concise maintainer instructions and validate release script routing plus signature verification and tamper rejection.

## Task 5: Integration and installed update

- [x] Review the three scoped P1 diffs and regression results; independent review found no unresolved high-confidence P1/P2.
- [x] Build production x64 payload and installer, verify embedded update identity and package contents.
- [x] Back up installed application/settings as appropriate, close Pulse gracefully, and perform the authorized in-place update while retaining user data.
- [x] Check the installed version, launch behavior and configured update source. Report any verification limits and provide installer/key-backup locations without exposing private key contents.

## Install-blocking follow-up

The first authorized in-place upgrade rolled back when the existing Start pin registration routine requested write access to every ancestor of its owned registry key. An elevated read-only access probe confirmed read access to `HKLM\Software` and `HKLM\Software\Classes`, and write access to the actual Pulse target keys, but not write access to those broad ancestors. The installed 1.0.60 core files still matched the backup after rollback.

- [x] Add a narrow synthetic registration regression, request only the rights needed for existing ancestors and owned leaf values, and preserve rollback ownership.
- [x] Rebuild/review the affected installer, re-sign and verify its manifest, then retry the in-place upgrade.
## Verified outcome

- Production x64 1.0.61 built without compiler diagnostics and passed package/resource checks.
- Delayed move source regression: 25/25. AI open authorization: 31/31; authority boundary: 25/25.
- Setup cleanup: 27/27; options: 8/8; handoff: 9/9; migration: 176/176; Start pin registration: 14/14.
- Update checker/installer/session: 50/50, 47/47, 8/8. Release pack routing: 18/18.
- Final generated manifest verified with the actual C++ verifier; a modified installer hash was rejected.
- In-place installation exited 0. Registry and executable report 1.0.61. Installed app, launcher and uninstaller hashes match the build. Setup preferences are preserved; PulseIndex is running; the installed UI opened successfully.
- Cloud Secret replacement is the one user-deferred step. No GitHub Release was published.

Validation limits: the new filesystem regressions use isolated local NTFS fixtures; Win8.1, UNC/SMB and ReFS runtime behavior was not exercised. Existing reboot deletion queues remain path-based, so identity is not pinned after scheduling until reboot. AI authorization binds the resolved path; same-path external replacement is not addressed by this change. No unrelated P2 findings were included.