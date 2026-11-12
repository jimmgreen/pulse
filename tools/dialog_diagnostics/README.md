# Dialog coverage diagnostics

`report.py` is a Python 3 standard-library CLI. It reads runtime JSONL (including
rotations/critical snapshots) and the managed runtime's older `interface=...`
TSV format. It never treats a software name, window title, or missing log as a
custom-drawn dialog or a successful takeover.

```powershell
python tools/dialog_diagnostics/report.py --logs bench_data/dialog-takeover-20261110 --output bench_data/dialog-next-20261110/diagnostics-historical-report.json
python -m unittest discover -s tools/dialog_diagnostics -p test_report.py
.\build\pulse_file_dialog_wrapper_test.exe --show-diagnostics
```

The first command is useful on existing evidence, not just synthetic fixtures:
the historical managed TSV has no process/object/Show correlation, so its
observations appear in `unattributed_reason_counts`, **not** as counted Shows.
The four x86/x64 × ordinary/managed cells remain `not_measured` unless there is
explicit evidence for that cell. A build/activation smoke line is not a picker
Show or a host-consumption result.

## Request correlation

Auto COM diagnostics emit `dialog_show_begin` and `dialog_show_terminal`:

```json
{"session":700,"pid":42,"seq":1,"event":"dialog_show_begin","data":{"dialog":6,"request":"6:1","show_sequence":1,"process_arch":64,"entry_family":"COM","launch_route":"ordinary"}}
{"session":700,"pid":42,"seq":2,"event":"dialog_show_terminal","data":{"dialog":6,"request":"6:1","show_sequence":1,"terminal_status":"cancelled","hr":2147943623,"provider_started":1,"ui_shown_known":0,"ui_shown":0,"reason_name":"None","fallback_reason":""}}
```

The key is `(logger session, pid, per-Show request)`, **not** logger session alone,
the COM object, a setter index, or a thread. Repeated Show on the same object gets
a new request. Begin, contract, and terminal records carry that request. Object-
only or thread-adjacent historical records are not guessed onto a Show. Setter
events do not add requests; duplicate snapshots do not add calls. A failure or
conflicting terminal cannot be overwritten by a later accepted record.

`ui_shown:null` means unknown. The wrapper's existing `shown_` flag means an
attempt and is never UI evidence. Only the V2 provider's explicit UI_SHOWN flag
supplies UI evidence; the basic bridge and native Show remain unknown. A native
S_OK is `native_accepted`, never `PULSE_ACCEPTED`. Close(S_OK) without an accepted
Shell result is `closed`, not acceptance. This observation seam does not change
admission, provider routing, return HRESULT, or fallback behavior. Observer
exceptions are swallowed, and reentrant Show remains busy without overwriting
the outer Show's observation.

## Controlled denominator

Pass `--expected-calls HOST_CALLS.json` only for a controlled test host's explicit
call inventory. The host must count planned real API calls independently of
received Pulse logs. Each call needs an exact process/logger/request identity;
if the real request never entered Pulse, the test harness must provide the
correlation identity reserved for that call. Do not construct the denominator
by enumerating received hook events. An uncorrelatable historical test has no
coverage denominator.

```json
{
  "controlled_host_count": true,
  "calls": [
    {"case_id":"open-1","logger_session":700,"pid":42,"request":"6:1","process_arch":"x64","launch_route":"ordinary","entry_family":"COM"}
  ]
}
```

Only explicitly listed calls with no associated runtime entry evidence become
`ENTRY_NOT_INSTRUMENTED`. With no manifest, entry coverage is `null`, not 0% or
100%. A correlated contract event without a terminal proves some entry evidence
but remains `TERMINAL_NOT_OBSERVED`. Incomplete or rotated logs can still make a
controlled observation inconclusive; retain the host and logger-health evidence
when interpreting a missing call.

## Host consumption and reason layers

An accepted picker result alone never verifies application behavior. The test
host must emit a separately associated record after it actually reads the open
result or writes and reopens the saved result:

```json
{"session":700,"pid":42,"event":"host_consumption","data":{"request":"6:1","evidence_source":"test_host","operation":"read","verified":true}}
```

For a separate logger, use `data.target_logger_session` and `data.target_pid` to
identify the runtime record. Only `read` or `write_reopen` with explicit verified
evidence and an accepted, nonfailed Pulse terminal set
`host_consumption_verified:true`. Cancellation, native completion, incomplete
evidence, and failure never inherit a prior Show's verified consumption.

`reasons_by_layer` separates entry, deployment, contract, provider and terminal
evidence. Fixed reason codes include component/registration mismatch, security
boundary, unsupported interfaces/options/callbacks, model capacity, startup
failure before UI, failure after explicit interaction, and unknown UI state.
Unrecognized fields and raw trace strings are not reflected. The report contains
opaque identifiers, fixed codes, numeric process identities and aggregate counts;
it excludes source paths, caller filenames, document contents and raw details.
Auto logging also omits module/provider/registration paths by default.

## Process snapshots and missing entry evidence

Settings' exported diagnostics can append `process_snapshot` JSONL after a text
header. Pass that exported `.txt` file explicitly with `--logs`; directory
discovery still selects JSONL/TSV files. These records describe a sampled process
in the collector's current context, **not a coverage denominator**, a Show, or
proof about the target's registration.

```json
{"event":"process_snapshot","pid":42,"data":{"snapshot_id":"42:133000000000000000","process_arch":64,"app_image_id":1234567,"creation_time_filetime":133000000000000000,"sample_tick_ms":1234,"process_query_error":0,"elevated":null,"elevation_error":5,"module_status":"access_denied","module_error":5,"loaded_components":[],"scope":"current_collector_context","evidence_scope":"observed_process_metadata_only"}}
```

The report's separate `process_observations` keeps only typed numeric/boolean
fields and fixed component identifiers (`auto`, `runtime`, `picker_ui`). Snapshot
and app-image identities are opaque; executable paths, module paths, window
titles, command lines and unrecognized strings are not reflected. The basename
fingerprint `app_image_id` is useful for local comparison, never Show correlation.
Module presence uses known component basenames only
(`module_identity_basis:known_component_names_only`); the collector does not
verify their hashes or signatures. A matching name is not trusted identity,
activation, enabled-state, or successful-interception proof.
PID, image fingerprint, and sampling proximity do not establish a logger session
or a particular process lifetime, so a snapshot remains
`NO_CORRELATED_ENTRY_EVIDENCE` even when another record has the same PID.

- `MODULE_SNAPSHOT_ACCESS_DENIED` means component presence is **unknown**. No
  runtime log plus access denied does not mean unloaded, legacy API, or custom UI.
- A partial enumeration may prove a listed component present; unlisted components
  stay unknown. Failed or inconsistent completeness does not prove absence.
- Only `module_status:complete`, zero module/process-query errors, and a known
  creation time allow `absent_from_complete_snapshot`. The collector must have
  reached `ERROR_NO_MORE_FILES`, checked unchanged creation time, and confirmed
  the process still alive. This is absence **at that sample**, not proof that a
  component was never loaded, an API was unused, or a dialog is custom-drawn.
- `token_rejected` is different: it is an observed security rejection, recorded
  separately in `entry_rejections`. Known `reason_name`/`token_reason` values and
  the older Auto numeric `reason` values 1–11 get the same fixed readable names.
  Unknown details remain `UNKNOWN`; inconsistent known names/numbers become
  `CONFLICTING_TOKEN_REASON`. Token metadata or an elevated flag alone never
  invents a rejection. Without an explicit correlated request, rejection records
  also add no Show and no coverage denominator.

Consequently, an uncorrelated snapshot leaves `entry_coverage:null` when no
controlled call inventory was supplied. It cannot fill a missing controlled
entry or change a matrix cell to measured. Request correlation, sampled module
presence and known rejection evidence remain separate explanations.

`collection_summaries` separately retains typed counts, limits, cancellation and
read errors from `process_snapshot_collection`, `dialog_log_collection`,
`managed_log_sources`, and collector-context `registration_snapshot`. Raw source
paths and unknown text are omitted. None of these summaries is a request or a
target registration conclusion. Explicit log-excerpt loss (including unavailable
managed sources) makes evidence incomplete and coverage `null`; a missing
controlled entry then stays `ENTRY_OBSERVATION_INCOMPLETE`. Errors collecting
process/registration metadata do not reduce Show-log coverage.

Actual records wrapped with `kind:dialog_log_excerpt` still use their original
event/session/PID/request identities. Within that exact correlated request,
numeric uint64 `seq` values order sequenced events across rotated files before
choosing the latest reason; unsequenced positions keep their input order. No
cross-session/PID or process-snapshot association is inferred.

`--family-status FILE.json` accepts independently verified implementation/SDK
readiness for `WINRT` and `WINDOWS_APP_SDK`. These records appear only in
`family_readiness`, never as observed API calls or an entry denominator. Fixed
reasons distinguish a missing WinRT storage-grant adapter, an unimplemented App
SDK picker adapter and a missing local development projection/dependency. An
installed App Runtime package alone is not a configured development SDK. See
`docs/dialog-picker-family-feasibility-20261110.md` for native probe evidence.

The CLI does not modify registrations, launch applications, enable interception,
or infer global coverage from ordinary COM coverage. Managed traditional APIs,
ordinary traditional APIs, x86 and x64 must be measured independently.

## Managed-launch preflight (B3)

The production launcher emits `target_preflight` to stdout, `launcher.log`, and
`launcher-preflight.jsonl` inside its owned session directory. The support export
retains only allowlisted counters and fixed warning identifiers. The report's
`launch_preflights` are separate from picker Shows: they never change the coverage
denominator or certify an existing process as taken over.

- Counts are confirmed lower bounds for the same-session, full executable path
  at the snapshot instant, not software-wide instance counts.
- Incomplete scans, query errors, disappearance, cancellation and timeout remain
  unknown; zero matches does not prove absence. Hard-link aliases are not inferred.
- Single-instance forwarding can target an old process. The new process's runtime
  handshake is not proof that the recipient uses Pulse.
- Scanning does not read commands/documents, attach to or stop existing processes,
  or block legitimate multi-instance applications.

Targeted regression: `python -m unittest discover -s tools/dialog_diagnostics -p test_launch_preflight.py`.
