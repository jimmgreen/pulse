"""Privacy-preserving, evidence-only file-dialog coverage report (stdlib only)."""

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import sys


BEGIN_EVENTS = {"dialog_show_begin", "show_begin", "request_begin"}
TERMINAL_EVENTS = {"dialog_show_terminal", "show_terminal", "request_terminal"}
REQUEST_EVENTS = BEGIN_EVENTS | TERMINAL_EVENTS | {"dialog_entry"}
ASSOCIATED_EVENTS = REQUEST_EVENTS | {"contract_decision", "observe_contract", "provider", "pulse_show", "pulse_result", "native", "native_result"}
REASON_LAYERS = {
    "ENTRY_NOT_INSTRUMENTED": "entry",
    "ENTRY_OBSERVATION_INCOMPLETE": "entry",
    "ENTRY_DISABLED_OR_UNAVAILABLE": "entry",
    "SECURITY_BOUNDARY": "entry",
    "NATIVE_OBJECT_ALREADY_CREATED": "entry",
    "ARCH_COMPONENT_MISSING": "deployment",
    "REGISTRATION_MISMATCH": "deployment",
    "UNSUPPORTED_INTERFACE": "contract",
    "UNSUPPORTED_OPTION_COMBINATION": "contract",
    "UNSUPPORTED_CALLBACK_OR_TEMPLATE": "contract",
    "MODEL_CAPACITY_EXCEEDED": "contract",
    "CONTRACT_SETTER_FAILED": "contract",
    "UNSUPPORTED_CONTRACT_FEATURE": "contract",
    "CONTRACT_INVALID_DATA": "contract",
    "INITIAL_FOLDER_UNREPRESENTABLE": "contract",
    "NATIVE_CONTRACT_FALLBACK": "contract",
    "PROVIDER_START_FAILED_BEFORE_UI": "provider",
    "PROVIDER_FAILED_AFTER_INTERACTION": "provider",
    "PROVIDER_FAILED_UI_STATE_UNKNOWN": "provider",
    "PULSE_ACCEPTED": "terminal",
    "PULSE_CANCELLED": "terminal",
    "PULSE_CLOSED_WITHOUT_RESULT": "terminal",
    "NATIVE_COMPLETED": "terminal",
    "NATIVE_FAILED": "terminal",
    "NATIVE_FALLBACK_REASON_NOT_OBSERVED": "terminal",
    "TERMINAL_NOT_OBSERVED": "terminal",
    "TERMINAL_CONFLICT": "terminal",
    "UNRECOGNIZED_TERMINAL": "terminal",
    "WINRT_STORAGE_GRANT_NOT_IMPLEMENTED": "contract",
    "APP_SDK_PICKER_ADAPTER_NOT_IMPLEMENTED": "contract",
    "APP_SDK_DEVELOPMENT_DEPENDENCY_MISSING": "deployment",
}
CONTRACT_REASONS = {
    "SetterFailed": "CONTRACT_SETTER_FAILED",
    "UnrecognizedSuccess": "CONTRACT_SETTER_FAILED",
    "UnknownSetting": "UNSUPPORTED_CONTRACT_FEATURE",
    "UnsupportedOptions": "UNSUPPORTED_OPTION_COMBINATION",
    "FilesystemNotForced": "UNSUPPORTED_OPTION_COMBINATION",
    "RecentItemsPolicy": "UNSUPPORTED_OPTION_COMBINATION",
    "MissingValidationOptions": "UNSUPPORTED_OPTION_COMBINATION",
    "Events": "UNSUPPORTED_CALLBACK_OR_TEMPLATE",
    "Customization": "UNSUPPORTED_CALLBACK_OR_TEMPLATE",
    "Limit": "MODEL_CAPACITY_EXCEEDED",
    "InvalidData": "CONTRACT_INVALID_DATA",
}
for _name in (
    "DefaultDirectoryPolicy", "NonFilesystemFolder", "UnsupportedFilename",
    "UnsupportedExtension", "UnsupportedFilter", "UnsupportedSettingOrder",
    "FolderFileSettings", "SaveContract", "Labels", "Places", "Persistence",
    "SaveMetadata", "ShellFilter",
):
    CONTRACT_REASONS[_name] = "UNSUPPORTED_CONTRACT_FEATURE"
ENTRY_FAMILIES = {
    "COM", "COM_OPEN", "COM_SAVE", "OFN_W", "OFN_A", "LEGACY_OPEN_W",
    "LEGACY_OPEN_A", "LEGACY_SAVE_W", "LEGACY_SAVE_A", "SHBROWSE_W",
    "SHBROWSE_A", "WINRT", "WINDOWS_APP_SDK",
}
LEGACY_REASONS = {
    "activation-adapter-entered": "ENTRY_OBSERVED_UNCORRELATED",
    "activation-session-disabled": "ENTRY_DISABLED_OR_UNAVAILABLE",
    "resolve-disabled-native": "ENTRY_DISABLED_OR_UNAVAILABLE",
    "resolve-not-ready-native": "ENTRY_DISABLED_OR_UNAVAILABLE",
    "inprocess-startup-native-fallback": "PROVIDER_START_FAILED_BEFORE_UI",
    "provider-load-on-demand-failed": "PROVIDER_START_FAILED_BEFORE_UI",
    "legacy-adapter-declined": "LEGACY_DECLINED_UNCORRELATED",
    "hook-not-supported": "UNSUPPORTED_CALLBACK_OR_TEMPLATE",
    "template-not-supported": "UNSUPPORTED_CALLBACK_OR_TEMPLATE",
}
FALLBACK_ALIASES = {
    "resolve-not-ready": "ENTRY_DISABLED_OR_UNAVAILABLE",
    "provider-unavailable": "PROVIDER_START_FAILED_BEFORE_UI",
    "initial-folder-unavailable": "INITIAL_FOLDER_UNREPRESENTABLE",
    "contract": "NATIVE_CONTRACT_FALLBACK",
    "provider-prepare-failed": "PROVIDER_START_FAILED_BEFORE_UI",
    "provider-failed-before-ui": "PROVIDER_START_FAILED_BEFORE_UI",
}
TOKEN_REASONS = dict(enumerate((
    "THREAD_IMPERSONATION", "THREAD_TOKEN_QUERY_FAILED", "PROCESS_TOKEN_QUERY_FAILED",
    "ELEVATION_QUERY_FAILED", "ELEVATED_BUNDLE_UNTRUSTED", "RESTRICTED_TOKEN",
    "APPCONTAINER_QUERY_FAILED", "APPCONTAINER", "INTEGRITY_QUERY_FAILED",
    "INVALID_INTEGRITY_SID", "BELOW_MEDIUM_INTEGRITY",
), 1))
PROCESS_COMPONENTS = ("auto", "runtime", "picker_ui")
COLLECTION_EVENTS = {"process_snapshot_collection", "dialog_log_collection", "managed_log_sources"}
COLLECTION_ERRORS = {"directory_error", "enumeration_error", "last_read_error", "registry_error", "component_error", "last_status_error"}
COLLECTION_COUNTS = set("snapshot_count collected unavailable_sources entries_scanned eligible_files files_read bytes_read lines_scanned records_emitted non_diagnostic_lines invalid_lines fields_omitted partial_lines_discarded oversized_lines oversized_files rejected_files tail_files max_files max_tail_bytes max_read_bytes max_line_bytes max_records max_output_bytes".split())
COLLECTION_FLAGS = set("incomplete cancelled callback_error limit_reached budget_exhausted enumeration_limited files_limited read_limited output_limited line_limited no_entry_inference process_snapshot_joined".split())


def opaque(value):
    """Never reflect a caller string which could be a document name or path."""
    if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
        return str(value)
    if isinstance(value, str) and re.fullmatch(r"[0-9]+(?::[0-9]+)?", value):
        return value
    return "id-" + hashlib.sha256(str(value).encode("utf-8")).hexdigest()[:16]


def integer(value):
    if isinstance(value, bool):
        return None
    try:
        return int(value, 0) if isinstance(value, str) else int(value)
    except (ValueError, TypeError, OverflowError):
        return None


def boolean(value):
    return value is True or (type(value) is int and value == 1)


def arch(value):
    return {"32": "x86", "64": "x64", "x86": "x86", "x64": "x64"}.get(str(value), "unknown")


def launch(value):
    return value if value in ("ordinary", "managed") else "unknown"


def family(value):
    return value if isinstance(value, str) and value in ENTRY_FAMILIES else "unknown"


def unsigned(value, bits=32):
    return value if type(value) is int and 0 <= value < (1 << bits) else None


def token_reason(data):
    """Names are a fixed vocabulary; numeric values belong to Auto's old gate."""
    number = data.get("reason")
    if isinstance(number, str) and re.fullmatch(r"[0-9]+", number):
        number = integer(number)
    number = unsigned(number)
    names = {data[field] for field in ("reason_name", "token_reason", "reason")
             if isinstance(data.get(field), str) and data[field] in TOKEN_REASONS.values()}
    if number in TOKEN_REASONS:
        names.add(TOKEN_REASONS[number])
    if len(names) > 1:
        return "CONFLICTING_TOKEN_REASON", number
    return next(iter(names), "UNKNOWN"), number


def process_observation(event):
    data = event.get("data", {})
    pid = unsigned(event.get("pid"))
    if not pid:
        return None
    creation = unsigned(data.get("creation_time_filetime"), 64)
    sample = data.get("sample_tick_ms")
    if type(sample) not in (int, float) or not 0 <= sample < (1 << 64) or not math.isfinite(sample):
        sample = None
    query_error = unsigned(data.get("process_query_error"))
    module_error = unsigned(data.get("module_error"))
    status = data.get("module_status")
    if status not in ("complete", "partial", "access_denied", "unavailable"):
        status = "unavailable"
    denied = status == "access_denied" or module_error == 5
    complete = status == "complete" and module_error == 0 and query_error == 0 and creation is not None
    observed = data.get("loaded_components")
    # A denied/failed enumeration cannot certify even an injected positive list.
    present = {name for name in observed if isinstance(name, str) and name in PROCESS_COMPONENTS} if isinstance(observed, list) and not denied and status in ("complete", "partial") else set()
    states = {name: "present" if name in present else "absent_from_complete_snapshot" if complete else "unknown"
              for name in PROCESS_COMPONENTS}
    if denied:
        evidence = "MODULE_SNAPSHOT_ACCESS_DENIED"
    elif complete:
        evidence = "LOADED_COMPONENT_ABSENT_FROM_COMPLETE_SNAPSHOT" if len(present) < len(PROCESS_COMPONENTS) else "LOADED_COMPONENTS_PRESENT"
    else:
        evidence = "MODULE_SNAPSHOT_PARTIAL" if status in ("complete", "partial") else "MODULE_SNAPSHOT_UNAVAILABLE"
    snapshot = data.get("snapshot_id")
    if snapshot is None and creation is not None:
        snapshot = f"{pid}:{creation}"
    image = unsigned(data.get("app_image_id"), 64)
    return {"snapshot_id": opaque(snapshot) if snapshot is not None else None,
            "pid": pid, "process_arch": arch(data.get("process_arch")),
            "app_image_id": opaque(image) if image is not None else None,
            "creation_time_filetime": creation, "sample_tick_ms": sample,
            "process_query_error": query_error,
            "elevated": data.get("elevated") if type(data.get("elevated")) is bool else None,
            "elevation_error": unsigned(data.get("elevation_error")),
            "module_status": status, "module_error": module_error,
            "module_identity_basis": "known_component_names_only",
            "component_states": states, "module_evidence": evidence,
            "entry_evidence": "NO_CORRELATED_ENTRY_EVIDENCE",
            "scope": "current_collector_context" if data.get("scope") == "current_collector_context" else "unknown",
            "evidence_scope": "observed_process_metadata_only",
            "counts_as_observed_request": False}


def entry_rejection(event, request_keys):
    data = event.get("data", {})
    key = identity(event)
    name, number = token_reason(data)
    session = event.get("session", event.get("logger_session"))
    return {"logger_session": opaque(session) if session is not None else None,
            "pid": unsigned(event.get("pid")),
            "request_id": key[2] if key in request_keys else None,
            "process_arch": arch(data.get("process_arch")),
            "entry_evidence": "REJECTED_KNOWN_REASON", "reason_code": "SECURITY_BOUNDARY",
            "token_reason": name, "token_reason_number": number,
            "error": unsigned(data.get("error")),
            "stage": data.get("stage") if data.get("stage") in ("activation", "resolve", "validation") else "unknown",
            "counts_as_observed_request": False}


def collection_summary(event):
    name, data = event.get("event"), event.get("data", {})
    if name not in COLLECTION_EVENTS and not (name == "registration_snapshot" and data.get("scope") == "current_collector_context"):
        return None
    def field(key):
        return data.get(key, event.get(key))
    scope = field("scope")
    row = {"event": name, "scope": scope if scope in (
        "visible_top_level_window_processes", "direct_whitelisted_log_excerpt", "current_collector_context"
    ) else "unknown", "counts_as_observed_request": False}
    for key in sorted(COLLECTION_ERRORS | COLLECTION_COUNTS | COLLECTION_FLAGS):
        if key not in data and key not in event:
            continue
        value = field(key)
        if key in COLLECTION_FLAGS:
            row[key] = value if type(value) is bool else None
        else:
            row[key] = unsigned(value, 32 if key in COLLECTION_ERRORS or key in ("collected", "unavailable_sources") else 64)
    if name == "registration_snapshot":
        row["process_arch"] = arch(field("process_arch"))
        row["registration"] = field("registration") if field("registration") in ("unregistered", "registered", "disabled_matching", "conflict", "read_error") else "unknown"
        row["component"] = field("component") if field("component") in ("unknown", "missing", "present") else "unknown"
    return row


def collection_log_loss(row):
    if row["event"] == "managed_log_sources":
        return row.get("limit_reached") is True or any((row.get(key) or 0) > 0 for key in ("unavailable_sources", "last_status_error"))
    if row["event"] != "dialog_log_collection":
        return False
    return any(row.get(key) is True for key in (
        "incomplete", "cancelled", "callback_error", "enumeration_limited", "files_limited", "read_limited", "output_limited", "line_limited"
    )) or any((row.get(key) or 0) > 0 for key in (
        "directory_error", "enumeration_error", "last_read_error", "invalid_lines", "partial_lines_discarded", "oversized_lines", "oversized_files", "rejected_files", "tail_files"
    ))


def identity(event):
    data = event.get("data", {})
    session = event.get("session", event.get("logger_session"))
    pid = integer(event.get("pid"))
    if event.get("event") == "host_consumption":
        session = data.get("target_logger_session", session)
        pid = integer(data.get("target_pid", pid))
    request = data.get("request", data.get("request_id"))
    if request in (None, "") and data.get("dialog") is not None and data.get("show_sequence") is not None:
        request = f"{data['dialog']}:{data['show_sequence']}"
    if request in (None, "") or session is None or pid is None or pid <= 0:
        return None
    return opaque(session), pid, opaque(request)


def parse_legacy_line(line, source):
    match = re.fullmatch(r"interface=(\S+) tick=(\d+) reason=(.*?) options=(0x[0-9a-fA-F]+) category=(\d+)\s*", line)
    if not match:
        return None
    entry, tick, reason, options, category = match.groups()
    code = LEGACY_REASONS.get(reason, "LEGACY_EVIDENCE_UNCORRELATED")
    return {"event": "legacy_evidence", "_source": source, "data": {"reason_code": code},
            "_legacy_dedup": hashlib.sha256(line.encode("utf-8")).hexdigest()}


def read_logs(inputs):
    events, malformed, skipped = [], 0, 0
    paths = set()
    for value in inputs:
        path = Path(value)
        if not path.exists():
            raise ValueError("a log input does not exist")
        if path.is_dir():
            paths.update(p for p in path.rglob("*") if p.is_file() and (p.suffix in (".jsonl", ".tsv") or ".jsonl." in p.name))
        else:
            paths.add(path)
    for path in sorted(paths):
        source = "source-" + hashlib.sha256(str(path.resolve()).encode("utf-8")).hexdigest()[:16]
        with path.open(encoding="utf-8-sig", errors="replace") as stream:
            for line in stream:
                if not line.strip():
                    continue
                if line.lstrip().startswith("{"):
                    try:
                        event = json.loads(line)
                        if not isinstance(event, dict) or not isinstance(event.get("event"), str) or not isinstance(event.get("data", {}), dict):
                            raise ValueError()
                        event["_source"] = source
                        events.append(event)
                    except (json.JSONDecodeError, ValueError):
                        malformed += 1
                else:
                    event = parse_legacy_line(line.strip(), source)
                    if event:
                        events.append(event)
                    elif line.startswith("interface="):
                        malformed += 1
                    else:
                        skipped += 1
    return events, {"sources": len(paths), "malformed_lines": malformed, "non_diagnostic_lines": skipped}


def controlled_calls(manifest):
    if manifest is None:
        return {}
    if manifest.get("controlled_host_count") is not True or not isinstance(manifest.get("calls"), list):
        raise ValueError("expected calls require controlled_host_count:true and a calls list")
    calls = {}
    for call in manifest["calls"]:
        if not isinstance(call, dict):
            raise ValueError("expected calls must be objects")
        item = {"session": call.get("logger_session"), "pid": call.get("pid"), "data": call}
        key = identity(item)
        if key is None:
            raise ValueError("each controlled call needs logger_session, pid and request")
        if key in calls:
            raise ValueError("duplicate controlled call identity")
        calls[key] = call
    return calls


def deduplicate(events):
    seen, unique, duplicates = set(), [], 0
    for event in events:
        data = event.get("data", {})
        if not isinstance(data, dict):
            continue
        if event.get("_legacy_dedup"):
            key = ("legacy", event.get("_source"), event["_legacy_dedup"])
        elif event.get("seq") is not None:
            key = ("sequence", opaque(event.get("session")), event.get("pid"), event["seq"])
        else:
            key = ("payload", json.dumps({k: v for k, v in event.items() if not k.startswith("_")}, sort_keys=True))
        if key in seen:
            duplicates += 1
        else:
            seen.add(key)
            unique.append(event)
    return unique, duplicates


def event_reasons(event):
    data = event.get("data", {})
    codes = []
    for field in ("reason_code", "fallback_reason"):
        code = data.get(field)
        if isinstance(code, str):
            code = FALLBACK_ALIASES.get(code, code)
        if isinstance(code, str) and code in REASON_LAYERS:
            codes.append(code)
    name = data.get("reason_name", data.get("contract_reason"))
    if isinstance(name, str) and name in CONTRACT_REASONS:
        codes.append(CONTRACT_REASONS[name])
    if event.get("event") == "token_rejected":
        codes.append("SECURITY_BOUNDARY")
    if event.get("event") == "bridge_trace" and data.get("detail") == "query-interface-not-supported":
        codes.append("UNSUPPORTED_INTERFACE")
    if event.get("event") in ("activation_gate", "registration_snapshot"):
        if boolean(data.get("conflict")) or data.get("path_match") in (0, False):
            codes.append("REGISTRATION_MISMATCH")
    return codes


def terminal(event):
    data = event.get("data", {})
    status = data.get("terminal_status")
    hr = integer(data.get("hr"))
    failed = hr is not None and bool(hr & 0x80000000)
    cancelled = hr is not None and (hr & 0xFFFFFFFF) == 0x800704C7
    native = boolean(data.get("native_fallback")) or status in ("native", "native_accepted", "native_cancelled", "native_failed")
    if native:
        explicit = data.get("native_result_class")
        result = "cancelled" if cancelled or explicit == "cancelled" else "failed" if failed or explicit == "failed" else "accepted" if hr == 0 or explicit == "accepted" else "unknown"
        return "native_" + result
    if status == "failed" or (failed and not cancelled):
        return "failed"
    if cancelled or status in ("cancelled", "pulse_cancelled"):
        return "cancelled"
    if status in ("accepted", "pulse_accepted") and (hr is None or hr == 0):
        return "accepted"
    if status == "closed":
        return "closed"
    return "unknown"


def request_row(key, events, expected):
    # A bounded export can read newer rotation files first. Order only numeric
    # seq slots inside this exact session/pid/request; unsequenced slots stay put.
    sequenced = iter(sorted((e for e in events if unsigned(e.get("seq"), 64) is not None), key=lambda e: e["seq"]))
    events = [next(sequenced) if unsigned(e.get("seq"), 64) is not None else e for e in events]
    session, pid, request = key
    row = {"logger_session": session, "pid": pid, "request_id": request,
           "case_id": opaque(expected.get("case_id", "unlisted")) if expected else None,
           "process_arch": arch(expected.get("process_arch")), "launch_route": launch(expected.get("launch_route")),
           "entry_family": family(expected.get("entry_family")), "expected": bool(expected),
           "entry_observed": bool(events), "object_id": None, "provider_started": False,
           "ui_shown": None, "terminal_status": "not_observed", "native_result_class": "unknown",
           "host_consumption_verified": False, "reasons_by_layer": {}}
    reasons, terminals, consumption = [], [], False
    token_reasons = set()
    for event in events:
        data = event.get("data", {})
        for field, converter in (("process_arch", arch), ("launch_route", launch), ("entry_family", family)):
            value = converter(data.get(field, event.get(field)))
            if value != "unknown":
                row[field] = value
        if data.get("dialog", data.get("object_id")) is not None:
            row["object_id"] = opaque(data.get("dialog", data.get("object_id")))
        row["provider_started"] |= boolean(data.get("provider_started"))
        if "ui_shown" in data and ("ui_shown_known" not in data or boolean(data["ui_shown_known"])):
            shown = boolean(data["ui_shown"])
            row["ui_shown"] = shown if row["ui_shown"] is None else row["ui_shown"] or shown
        reasons.extend(event_reasons(event))
        if event.get("event") == "token_rejected":
            token_reasons.add(token_reason(data)[0])
        if event.get("event") in TERMINAL_EVENTS:
            terminals.append(terminal(event))
        if event.get("event") == "host_consumption" and data.get("evidence_source") == "test_host" and data.get("operation") in ("read", "write_reopen") and boolean(data.get("verified")):
            consumption = True
    unique_terminals = set(terminals)
    if "failed" in unique_terminals:
        row["terminal_status"] = "failed"
    elif len(unique_terminals) > 1:
        row["terminal_status"] = "failed"
        reasons.append("TERMINAL_CONFLICT")
    elif terminals:
        row["terminal_status"] = terminals[0]
    status = row["terminal_status"]
    if status.startswith("native_"):
        row["native_result_class"] = status[7:]
    row["host_consumption_verified"] = status == "accepted" and consumption
    if not events:
        reason = "ENTRY_NOT_INSTRUMENTED"
    elif status == "failed" and "TERMINAL_CONFLICT" in reasons:
        reason = "TERMINAL_CONFLICT"
    elif status == "failed":
        reason = "PROVIDER_FAILED_AFTER_INTERACTION" if row["ui_shown"] is True else "PROVIDER_START_FAILED_BEFORE_UI" if row["ui_shown"] is False else "PROVIDER_FAILED_UI_STATE_UNKNOWN"
    elif status == "accepted":
        reason = "PULSE_ACCEPTED"
    elif status == "cancelled":
        reason = "PULSE_CANCELLED"
    elif status == "closed":
        reason = "PULSE_CLOSED_WITHOUT_RESULT"
    elif status.startswith("native_"):
        specific = [r for r in reasons if r != "NATIVE_CONTRACT_FALLBACK"]
        reason = specific[-1] if specific else "NATIVE_CONTRACT_FALLBACK" if reasons else "NATIVE_FALLBACK_REASON_NOT_OBSERVED"
    elif status == "not_observed":
        reason = "TERMINAL_NOT_OBSERVED"
    else:
        reason = "UNRECOGNIZED_TERMINAL"
    row["reason_code"], row["layer"] = reason, REASON_LAYERS[reason]
    row["token_reasons"] = sorted(token_reasons)
    reasons.append(reason)
    if status in ("native_accepted", "native_cancelled"):
        reasons.append("NATIVE_COMPLETED")
    elif status == "native_failed":
        reasons.append("NATIVE_FAILED")
    for code in sorted(set(reasons)):
        row["reasons_by_layer"].setdefault(REASON_LAYERS[code], []).append(code)
    return row


def family_readiness(status):
    if status is None:
        return []
    if not isinstance(status, dict) or not isinstance(status.get("families"), list):
        raise ValueError("family status requires a families list")
    result = []
    for value in status["families"]:
        if not isinstance(value, dict):
            raise ValueError("family status entries must be objects")
        entry, code = value.get("entry_family"), value.get("reason_code")
        state, source = value.get("status"), value.get("evidence_source")
        if entry not in ("WINRT", "WINDOWS_APP_SDK") or code not in (
            "WINRT_STORAGE_GRANT_NOT_IMPLEMENTED", "APP_SDK_PICKER_ADAPTER_NOT_IMPLEMENTED", "APP_SDK_DEVELOPMENT_DEPENDENCY_MISSING"
        ) or state not in ("not_implemented", "development_dependency_missing") or source not in ("source_review", "local_sdk_inventory"):
            raise ValueError("family status uses an unrecognized fixed identifier")
        result.append({"entry_family": entry, "status": state, "reason_code": code,
                       "layer": REASON_LAYERS[code], "evidence_source": source,
                       "counts_as_observed_request": False})
    return result


def launch_preflight(event):
    data = event.get("data", {})
    count = unsigned(data.get("existing_target_count"), 32)
    errors = unsigned(data.get("query_errors"), 32)
    vanished = unsigned(data.get("vanished_processes", 0), 32)
    complete = boolean(data.get("scan_complete")) and errors == 0 and vanished == 0 and not boolean(data.get("cancelled")) and not boolean(data.get("timed_out"))
    warning = "existing_target" if count is not None and count > 0 else "none" if count == 0 and complete else "scan_incomplete"
    return {"logger_session": opaque(event.get("session")), "launcher_pid": unsigned(event.get("pid"), 32),
            "existing_target_count": count, "scan_complete": complete, "query_errors": errors,
            "vanished_processes": vanished, "cancelled": boolean(data.get("cancelled")),
            "timed_out": boolean(data.get("timed_out")),
            "scanned_processes": unsigned(data.get("scanned_processes"), 32),
            "same_session_processes": unsigned(data.get("same_session_processes"), 32),
            "warning": warning, "existing_target_managed": "unknown", "certified": False,
            "counts_as_observed_request": False}


def build_report(events, manifest=None, input_summary=None, family_status=None):
    expected = controlled_calls(manifest)
    unique, duplicate_count = deduplicate(events)
    keys = {identity(e) for e in unique if e.get("event") in REQUEST_EVENTS}
    keys.discard(None)
    keys.update(identity(e) for e in unique if e.get("event") in ASSOCIATED_EVENTS and identity(e) in expected)
    groups = {key: [] for key in keys | expected.keys()}
    snapshots, collections, preflights = [], [], []
    unattributed, reason_counts = 0, Counter()
    for event in unique:
        if event.get("event") == "target_preflight":
            preflights.append(launch_preflight(event))
            continue
        collection = collection_summary(event)
        if collection is not None:
            collections.append(collection)
            continue
        if event.get("event") == "process_snapshot":
            observation = process_observation(event)
            if observation is not None:
                snapshots.append(observation)
            continue
        key = identity(event)
        if key in groups and key in keys:
            groups[key].append(event)
        else:
            unattributed += 1
            codes = event_reasons(event)
            if event.get("event") == "legacy_evidence":
                code = event.get("data", {}).get("reason_code")
                codes = [code] if code in set(LEGACY_REASONS.values()) | {"LEGACY_EVIDENCE_UNCORRELATED"} else []
            reason_counts.update(codes or ["UNATTRIBUTED_DIAGNOSTIC_EVIDENCE"])
    rows = [request_row(key, groups[key], expected.get(key, {})) for key in sorted(groups)]
    incomplete = any(collection_log_loss(row) for row in collections) or bool(input_summary and input_summary.get("malformed_lines")) or any(
        any((integer(e.get("data", {}).get(k)) or 0) > 0 for k in ("dropped_events", "write_errors"))
        for e in unique if e.get("event") in ("process_health", "process_stop")
    )
    if incomplete:
        for row in rows:
            if not row["entry_observed"]:
                row["reason_code"] = "ENTRY_OBSERVATION_INCOMPLETE"
                row["reasons_by_layer"] = {"entry": ["ENTRY_OBSERVATION_INCOMPLETE"]}
    observed_expected = sum(row["expected"] and row["entry_observed"] for row in rows)
    metrics = {
        "controlled_expected_calls": len(expected) if manifest is not None else None,
        "controlled_entry_observed": observed_expected if manifest is not None else None,
        "entry_coverage": observed_expected / len(expected) if expected and not incomplete else None,
        "diagnostic_evidence_incomplete": incomplete,
        "observed_requests": len(keys), "entry_not_instrumented": sum(r["reason_code"] == "ENTRY_NOT_INSTRUMENTED" for r in rows),
        "pulse_accepted": sum(r["terminal_status"] == "accepted" for r in rows),
        "pulse_cancelled": sum(r["terminal_status"] == "cancelled" for r in rows),
        "pulse_failed": sum(r["terminal_status"] == "failed" for r in rows),
        "native_completed": sum(r["terminal_status"] in ("native_accepted", "native_cancelled") for r in rows),
        "host_consumption_verified": sum(r["host_consumption_verified"] for r in rows),
    }
    cells = {(a, r) for a in ("x86", "x64") for r in ("ordinary", "managed")}
    cells.update((r["process_arch"], r["launch_route"]) for r in rows)
    matrix = []
    for a, route in sorted(cells):
        matching = [r for r in rows if r["process_arch"] == a and r["launch_route"] == route]
        matrix.append({"process_arch": a, "launch_route": route,
                       "expected_calls": sum(r["expected"] for r in matching) if manifest is not None else None,
                       "observed_requests": sum(r["entry_observed"] for r in matching),
                       "entry_not_instrumented": sum(r["reason_code"] == "ENTRY_NOT_INSTRUMENTED" for r in matching),
                       "pulse_accepted": sum(r["terminal_status"] == "accepted" for r in matching),
                       "host_consumption_verified": sum(r["host_consumption_verified"] for r in matching),
                       "status": "observed" if matching else "not_measured"})
    summary = {"sources": len({e.get("_source") for e in unique if e.get("_source")}), "malformed_lines": 0, "non_diagnostic_lines": 0}
    summary.update(input_summary or {})
    summary.update({"parsed_events": len(events), "duplicate_events": duplicate_count, "unattributed_events": unattributed})
    summary["process_snapshot_events"] = sum(e.get("event") == "process_snapshot" for e in unique)
    summary["collection_summary_events"] = len(collections)
    return {"schema": 1, "input_summary": summary, "metrics": metrics, "matrix": matrix,
            "family_readiness": family_readiness(family_status),
            "process_observations": snapshots,
            "launch_preflights": preflights,
            "collection_summaries": collections,
            "entry_rejections": [entry_rejection(e, keys) for e in unique if e.get("event") == "token_rejected"],
            "reason_counts": dict(sorted(Counter(r["reason_code"] for r in rows).items())),
            "unattributed_reason_counts": dict(sorted(reason_counts.items())), "requests": rows,
            "interpretation": {
                "entry_denominator": "controlled_expected_calls_only",
                "legacy_evidence": "uncorrelated observations are not requests or coverage",
                "missing_evidence": "not evidence of custom UI, native success, or Pulse success",
                "process_snapshots": "sampled metadata only; no request correlation by pid or app_image_id; never a coverage denominator",
                "ui_shown": "null means no explicit provider UI evidence",
                "consumption": "explicit associated test_host read or write_reopen only",
                "privacy": "no source paths, document names, raw details, or unrecognized text",
            }}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--logs", action="append", required=True, help="JSONL/legacy TSV file or directory; repeatable")
    parser.add_argument("--expected-calls", help="controlled host call manifest, never inferred from log absence")
    parser.add_argument("--family-status", help="explicit source/development readiness evidence, never a measured request")
    parser.add_argument("--output", required=True, help="report JSON output")
    args = parser.parse_args(argv)
    try:
        events, summary = read_logs(args.logs)
        manifest = json.loads(Path(args.expected_calls).read_text(encoding="utf-8-sig")) if args.expected_calls else None
        status = json.loads(Path(args.family_status).read_text(encoding="utf-8-sig")) if args.family_status else None
        result = build_report(events, manifest, summary, status)
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    except (OSError, ValueError, TypeError) as error:
        # OSError text can contain paths; never reflect it into a shared report.
        print("diagnostics report failed: " + (str(error) if isinstance(error, ValueError) and not isinstance(error, json.JSONDecodeError) else type(error).__name__), file=sys.stderr)
        return 2
    m = result["metrics"]
    print(f"REPORT requests={m['observed_requests']} expected={m['controlled_expected_calls']} accepted={m['pulse_accepted']} consumed={m['host_consumption_verified']} unattributed={result['input_summary']['unattributed_events']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
