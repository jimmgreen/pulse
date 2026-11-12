import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPORT = Path(__file__).with_name("report.py")
SPEC = importlib.util.spec_from_file_location("dialog_diagnostics_report", REPORT)
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


def event(name, request=1, **data):
    data.setdefault("request", request)
    return {"session": 70, "pid": 42, "event": name, "data": data}


def expected(request=1, **extra):
    return {"controlled_host_count": True, "calls": [{
        "case_id": "com-open", "logger_session": 70, "pid": 42,
        "request": request, "process_arch": "x64", "launch_route": "ordinary",
        "entry_family": "COM", **extra,
    }]}


class ReportTests(unittest.TestCase):
    def make(self, events, manifest=None):
        return report.build_report(events, manifest)

    def test_no_log_is_unknown_without_controlled_denominator(self):
        result = self.make([])
        self.assertEqual(result["requests"], [])
        self.assertIsNone(result["metrics"]["entry_coverage"])
        self.assertEqual(result["metrics"]["entry_not_instrumented"], 0)

    def test_controlled_missing_call_is_entry_not_instrumented(self):
        result = self.make([], expected())
        row = result["requests"][0]
        self.assertEqual(row["reason_code"], "ENTRY_NOT_INSTRUMENTED")
        self.assertEqual(row["layer"], "entry")
        self.assertEqual(result["metrics"]["entry_coverage"], 0)

    def test_uncontrolled_expected_manifest_is_rejected(self):
        manifest = expected()
        manifest["controlled_host_count"] = False
        with self.assertRaises(ValueError):
            self.make([], manifest)

    def test_duplicate_setters_and_critical_events_do_not_count_calls(self):
        begin = event("dialog_show_begin", dialog=6, process_arch="x64", launch_route="ordinary")
        begin["seq"] = 1
        terminal = event("dialog_show_terminal", dialog=6, terminal_status="cancelled", hr=0x800704C7)
        terminal["seq"] = 3
        setter = event("contract_decision", dialog=6, reason_name="None")
        setter["seq"] = 2
        result = self.make([begin, setter, setter, terminal, terminal], expected())
        self.assertEqual(len(result["requests"]), 1)
        self.assertEqual(result["metrics"]["pulse_cancelled"], 1)
        self.assertEqual(result["metrics"]["controlled_entry_observed"], 1)
        self.assertEqual(result["input_summary"]["duplicate_events"], 2)

    def test_same_object_repeated_show_has_separate_request(self):
        result = self.make([
            event("dialog_show_begin", request=1, dialog=6),
            event("dialog_show_terminal", request=1, dialog=6, terminal_status="cancelled"),
            event("dialog_show_begin", request=2, dialog=6),
            event("dialog_show_terminal", request=2, dialog=6, terminal_status="accepted"),
        ])
        self.assertEqual(len(result["requests"]), 2)
        self.assertEqual(result["metrics"]["pulse_accepted"], 1)
        self.assertEqual(result["metrics"]["pulse_cancelled"], 1)

    def test_native_fallback_not_counted_as_pulse_success(self):
        result = self.make([
            event("dialog_show_begin"),
            event("contract_decision", reason_name="UnsupportedOptions"),
            event("dialog_show_terminal", terminal_status="native", native_result_class="accepted", hr=0),
        ])
        row = result["requests"][0]
        self.assertEqual(row["reason_code"], "UNSUPPORTED_OPTION_COMBINATION")
        self.assertEqual(row["terminal_status"], "native_accepted")
        self.assertEqual(result["metrics"]["pulse_accepted"], 0)
        self.assertEqual(result["metrics"]["native_completed"], 1)

    def test_cancel_does_not_reuse_prior_acceptance_on_same_object(self):
        result = self.make([
            event("dialog_show_begin", request=1, dialog=3),
            event("dialog_show_terminal", request=1, dialog=3, terminal_status="accepted"),
            event("host_consumption", request=1, dialog=3, evidence_source="test_host", operation="read", verified=True),
            event("dialog_show_begin", request=2, dialog=3),
            event("dialog_show_terminal", request=2, dialog=3, terminal_status="cancelled"),
        ])
        self.assertTrue(result["requests"][0]["host_consumption_verified"])
        self.assertFalse(result["requests"][1]["host_consumption_verified"])

    def test_failure_cannot_be_overwritten_by_later_success(self):
        result = self.make([
            event("dialog_show_begin"),
            event("dialog_show_terminal", terminal_status="failed", ui_shown=True, hr=0x80004005),
            event("dialog_show_terminal", terminal_status="accepted", hr=0),
            event("host_consumption", evidence_source="test_host", operation="read", verified=True),
        ])
        self.assertEqual(result["requests"][0]["terminal_status"], "failed")
        self.assertEqual(result["requests"][0]["reason_code"], "PROVIDER_FAILED_AFTER_INTERACTION")
        self.assertEqual(result["metrics"]["pulse_accepted"], 0)
        self.assertFalse(result["requests"][0]["host_consumption_verified"])

    def test_hresult_failure_cannot_be_named_accepted(self):
        result = self.make([event("dialog_show_begin"), event("dialog_show_terminal", terminal_status="accepted", hr=0x80004005)])
        self.assertEqual(result["requests"][0]["terminal_status"], "failed")
        self.assertEqual(result["metrics"]["pulse_accepted"], 0)

    def test_consumption_requires_explicit_test_host_read_or_reopen(self):
        for evidence in [
            {"verified": True},
            {"evidence_source": "runtime", "operation": "read", "verified": True},
            {"evidence_source": "test_host", "operation": "selected", "verified": True},
            {"evidence_source": "test_host", "operation": "read", "verified": False},
        ]:
            result = self.make([event("dialog_show_begin"), event("dialog_show_terminal", terminal_status="accepted"), event("host_consumption", **evidence)])
            self.assertFalse(result["requests"][0]["host_consumption_verified"])

    def test_process_identity_prevents_cross_process_request_collision(self):
        other = event("dialog_show_begin", process_arch="x86", launch_route="managed")
        other["pid"] = 43
        result = self.make([event("dialog_show_begin", process_arch="x64", launch_route="ordinary"), other])
        self.assertEqual(len(result["requests"]), 2)
        cells = {(r["process_arch"], r["launch_route"]): r["observed_requests"] for r in result["matrix"]}
        self.assertEqual(cells[("x64", "ordinary")], 1)
        self.assertEqual(cells[("x86", "managed")], 1)

    def test_unattributed_legacy_evidence_never_creates_requests(self):
        legacy = report.parse_legacy_line("interface=GetOpenFileNameW tick=4 reason=legacy-adapter-declined options=0x00000000 category=0", "source-1")
        result = self.make([legacy])
        self.assertEqual(result["requests"], [])
        self.assertEqual(result["input_summary"]["unattributed_events"], 1)
        self.assertIsNone(result["metrics"]["entry_coverage"])

    def test_provider_start_failure_is_distinct_from_post_interaction_failure(self):
        result = self.make([event("dialog_show_begin"), event("dialog_show_terminal", terminal_status="failed", ui_shown=False, hr=0x80004005)])
        self.assertEqual(result["requests"][0]["reason_code"], "PROVIDER_START_FAILED_BEFORE_UI")

    def test_deployment_and_security_reasons_remain_separate(self):
        for reason, layer in [("ARCH_COMPONENT_MISSING", "deployment"), ("REGISTRATION_MISMATCH", "deployment"), ("SECURITY_BOUNDARY", "entry")]:
            result = self.make([event("dialog_show_begin"), event("dialog_show_terminal", terminal_status="native", reason_code=reason, hr=0)])
            self.assertEqual(result["requests"][0]["layer"], layer)
            self.assertEqual(result["requests"][0]["reason_code"], reason)

    def test_unknown_text_and_paths_are_not_reflected(self):
        secret = r"C:\Users\private\document-secret.txt"
        result = self.make([event("dialog_show_begin", process_arch=secret, entry_family=secret), event("bridge_trace", detail=secret), event("dialog_show_terminal", terminal_status="accepted", reason_code=secret)], expected(case_id=secret))
        self.assertNotIn(secret, json.dumps(result))
        self.assertNotIn("document-secret", json.dumps(result))

    def test_object_only_setter_does_not_attach_to_arbitrary_show(self):
        result = self.make([event("dialog_show_begin", dialog=6), {"session": 70, "pid": 42, "event": "contract_decision", "data": {"dialog": 6, "reason_name": "UnsupportedOptions"}}])
        self.assertEqual(result["requests"][0]["reason_code"], "TERMINAL_NOT_OBSERVED")

    def test_duplicate_expected_identity_is_rejected(self):
        manifest = expected()
        manifest["calls"].append(dict(manifest["calls"][0]))
        with self.assertRaises(ValueError):
            self.make([], manifest)

    def test_expected_associated_setter_proves_entry_not_missing(self):
        result = self.make([event("contract_decision", reason_name="None")], expected())
        self.assertEqual(result["metrics"]["controlled_entry_observed"], 1)
        self.assertEqual(result["requests"][0]["reason_code"], "TERMINAL_NOT_OBSERVED")

    def test_unknown_ui_state_does_not_claim_pre_ui_fallback(self):
        result = self.make([event("dialog_show_begin"), event("dialog_show_terminal", terminal_status="failed", ui_shown_known=0, ui_shown=0, hr=0x80004005)])
        self.assertIsNone(result["requests"][0]["ui_shown"])
        self.assertEqual(result["requests"][0]["reason_code"], "PROVIDER_FAILED_UI_STATE_UNKNOWN")

    def test_conflicting_cancel_and_acceptance_are_not_success(self):
        result = self.make([event("dialog_show_begin"), event("dialog_show_terminal", terminal_status="cancelled"), event("dialog_show_terminal", terminal_status="accepted")])
        self.assertEqual(result["requests"][0]["reason_code"], "TERMINAL_CONFLICT")
        self.assertEqual(result["metrics"]["pulse_accepted"], 0)

    def test_combined_auto_request_string_correlates_begin_terminal(self):
        result = self.make([event("dialog_show_begin", request="6:2", dialog=6, show_sequence=2, process_arch=64), event("dialog_show_terminal", request="6:2", dialog=6, terminal_status="native", fallback_reason="NATIVE_CONTRACT_FALLBACK", reason_name="ShellFilter", hr=0x800704C7)])
        self.assertEqual(len(result["requests"]), 1)
        self.assertEqual(result["requests"][0]["request_id"], "6:2")
        self.assertEqual(result["requests"][0]["process_arch"], "x64")
        self.assertEqual(result["requests"][0]["reason_code"], "UNSUPPORTED_CONTRACT_FEATURE")

    def test_family_readiness_does_not_become_observed_or_missing_entry(self):
        result = report.build_report([], family_status={"families": [{
            "entry_family": "WINRT", "reason_code": "WINRT_STORAGE_GRANT_NOT_IMPLEMENTED",
            "status": "not_implemented", "evidence_source": "source_review",
        }, {
            "entry_family": "WINDOWS_APP_SDK", "reason_code": "APP_SDK_DEVELOPMENT_DEPENDENCY_MISSING",
            "status": "development_dependency_missing", "evidence_source": "local_sdk_inventory",
        }]})
        self.assertEqual(result["metrics"]["observed_requests"], 0)
        self.assertEqual(result["metrics"]["entry_not_instrumented"], 0)
        self.assertIsNone(result["metrics"]["entry_coverage"])
        self.assertEqual(len(result["family_readiness"]), 2)

    def test_family_status_rejects_raw_private_strings(self):
        with self.assertRaises(ValueError):
            report.build_report([], family_status={"families": [{"entry_family": r"C:\private\document.txt"}]})

    def test_pre_show_unknown_interface_is_uncorrelated_not_a_show(self):
        result = self.make([{"session": 70, "pid": 42, "event": "bridge_trace", "data": {"dialog": 6, "detail": "query-interface-not-supported"}}])
        self.assertEqual(result["metrics"]["observed_requests"], 0)
        self.assertEqual(result["unattributed_reason_counts"]["UNSUPPORTED_INTERFACE"], 1)

    def test_managed_enum_fallback_maps_to_provider_layer(self):
        result = self.make([event("dialog_show_begin"), event("dialog_show_terminal", terminal_status="native", fallback_reason="provider-prepare-failed", hr=0x800704C7)])
        self.assertEqual(result["requests"][0]["reason_code"], "PROVIDER_START_FAILED_BEFORE_UI")

    def test_reported_log_loss_prevents_missing_entry_claim(self):
        result = self.make([{"session": 70, "pid": 42, "event": "process_stop", "data": {"dropped_events": 1}}], expected())
        self.assertEqual(result["requests"][0]["reason_code"], "ENTRY_OBSERVATION_INCOMPLETE")
        self.assertIsNone(result["metrics"]["entry_coverage"])
        self.assertEqual(result["metrics"]["entry_not_instrumented"], 0)

    def test_cli_real_file_and_redacted_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "private-document.jsonl"
            source.write_text("\n".join(json.dumps(e) for e in [event("dialog_show_begin"), event("dialog_show_terminal", terminal_status="cancelled")]) + "\n", encoding="utf-8")
            output = root / "report.json"
            completed = subprocess.run([sys.executable, str(REPORT), "--logs", str(source), "--output", str(output)], capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            data = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(data["metrics"]["pulse_cancelled"], 1)
            self.assertNotIn("private-document", output.read_text(encoding="utf-8"))


class ProcessEvidenceTests(unittest.TestCase):
    def snapshot(self, **extra):
        data = {"snapshot_id": "42:133000000000000000", "process_arch": 64,
                "creation_time_filetime": 133000000000000000, "sample_tick_ms": 1234,
                "process_query_error": 0, "elevated": False, "elevation_error": 0,
                "app_image_id": 1234567, "module_status": "complete", "module_error": 0,
                "loaded_components": [], "scope": "current_collector_context",
                "evidence_scope": "observed_process_metadata_only", **extra}
        return {"event": "process_snapshot", "pid": 42, "data": data}

    def test_denied_snapshot_is_unknown_not_not_loaded_or_a_call(self):
        result = report.build_report([self.snapshot(module_status="access_denied", module_error=5)])
        row = result["process_observations"][0]
        self.assertEqual(row["entry_evidence"], "NO_CORRELATED_ENTRY_EVIDENCE")
        self.assertEqual(row["module_evidence"], "MODULE_SNAPSHOT_ACCESS_DENIED")
        self.assertEqual(set(row["component_states"].values()), {"unknown"})
        self.assertEqual(result["requests"], [])
        self.assertEqual(result["metrics"]["observed_requests"], 0)
        self.assertIsNone(result["metrics"]["entry_coverage"])
        self.assertFalse(row["counts_as_observed_request"])

    def test_complete_successful_snapshot_proves_only_sampled_absence(self):
        row = report.build_report([self.snapshot(loaded_components=["auto"])])["process_observations"][0]
        self.assertEqual(row["component_states"], {"auto": "present", "runtime": "absent_from_complete_snapshot", "picker_ui": "absent_from_complete_snapshot"})
        self.assertEqual(row["module_evidence"], "LOADED_COMPONENT_ABSENT_FROM_COMPLETE_SNAPSHOT")
        self.assertEqual(row["module_identity_basis"], "known_component_names_only")
        self.assertEqual(row["scope"], "current_collector_context")
        self.assertEqual(row["evidence_scope"], "observed_process_metadata_only")

    def test_partial_and_inconsistent_complete_never_prove_absence(self):
        for extra in ({"module_status": "partial"}, {"module_error": 299},
                      {"process_query_error": 5}, {"creation_time_filetime": None}):
            row = report.build_report([self.snapshot(loaded_components=["auto"], **extra)])["process_observations"][0]
            self.assertNotIn("absent_from_complete_snapshot", row["component_states"].values())
        partial = report.build_report([self.snapshot(module_status="partial", loaded_components=["auto"])])["process_observations"][0]
        self.assertEqual(partial["component_states"]["auto"], "present")
        self.assertEqual(partial["component_states"]["runtime"], "unknown")

    def test_snapshot_does_not_supply_missing_controlled_entry(self):
        snap = self.snapshot()
        snap["session"] = 70
        snap["data"]["request"] = 1
        result = report.build_report([snap], expected())
        self.assertEqual(result["metrics"]["controlled_entry_observed"], 0)
        self.assertEqual(result["metrics"]["entry_coverage"], 0)
        self.assertEqual(result["requests"][0]["reason_code"], "ENTRY_NOT_INSTRUMENTED")

    def test_same_pid_and_image_do_not_correlate_process_lifetimes_or_shows(self):
        first = self.snapshot()
        second = self.snapshot(snapshot_id="42:134000000000000000", creation_time_filetime=134000000000000000)
        result = report.build_report([first, second, event("dialog_show_begin")])
        self.assertEqual(len(result["process_observations"]), 2)
        self.assertTrue(all(row["entry_evidence"] == "NO_CORRELATED_ENTRY_EVIDENCE" for row in result["process_observations"]))
        self.assertEqual(result["metrics"]["observed_requests"], 1)
        self.assertIsNone(result["metrics"]["entry_coverage"])

    def test_snapshot_typed_fields_and_private_strings_are_not_reflected(self):
        secret = r"C:\private\document-secret.txt"
        row = report.build_report([self.snapshot(snapshot_id=secret, app_image_id=secret,
            elevated=1, elevation_error=secret, process_query_error=True,
            creation_time_filetime=-1, sample_tick_ms=float("nan"), module_status=secret,
            module_error=secret, loaded_components=[secret], executable=secret, title=secret)])
        snapshot = row["process_observations"][0]
        for field in ("app_image_id", "elevated", "elevation_error", "process_query_error", "creation_time_filetime", "sample_tick_ms", "module_error"):
            self.assertIsNone(snapshot[field], field)
        self.assertNotIn("document-secret", json.dumps(row))
        self.assertNotIn("NaN", json.dumps(row))

    def test_readable_token_reason_matches_old_numeric_reason_without_a_show(self):
        names = ["THREAD_IMPERSONATION", "THREAD_TOKEN_QUERY_FAILED", "PROCESS_TOKEN_QUERY_FAILED",
                 "ELEVATION_QUERY_FAILED", "ELEVATED_BUNDLE_UNTRUSTED", "RESTRICTED_TOKEN",
                 "APPCONTAINER_QUERY_FAILED", "APPCONTAINER", "INTEGRITY_QUERY_FAILED",
                 "INVALID_INTEGRITY_SID", "BELOW_MEDIUM_INTEGRITY"]
        for number, name in enumerate(names, 1):
            for data in ({"reason": number}, {"reason_name": name}, {"reason": number, "reason_name": name}):
                result = report.build_report([{"session": 70, "pid": 42, "event": "token_rejected", "data": data}])
                rejection = result["entry_rejections"][0]
                self.assertEqual(rejection["token_reason"], name)
                self.assertEqual(rejection["reason_code"], "SECURITY_BOUNDARY")
                self.assertEqual(result["requests"], [])
                self.assertIsNone(result["metrics"]["entry_coverage"])

    def test_unknown_or_conflicting_token_details_do_not_invent_a_reason(self):
        secret = r"C:\private\token-secret.txt"
        for data in ({"reason": 999}, {"reason": True}, {"reason_name": secret},
                     {"reason": 1, "reason_name": "RESTRICTED_TOKEN"}):
            result = report.build_report([{"session": 70, "pid": 42, "event": "token_rejected", "data": data}])
            self.assertIn(result["entry_rejections"][0]["token_reason"], ("UNKNOWN", "CONFLICTING_TOKEN_REASON"))
            self.assertNotIn("token-secret", json.dumps(result))

    def test_known_rejection_requires_explicit_request_correlation(self):
        rejection = event("token_rejected", reason=6)
        result = report.build_report([event("dialog_show_begin"), rejection])
        self.assertEqual(result["entry_rejections"][0]["request_id"], "1")
        self.assertEqual(result["requests"][0]["token_reasons"], ["RESTRICTED_TOKEN"])
        orphan = report.build_report([rejection])
        self.assertIsNone(orphan["entry_rejections"][0]["request_id"])
        self.assertEqual(orphan["metrics"]["observed_requests"], 0)

    def test_export_txt_header_and_process_snapshot_are_readable_without_leaking(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "private-export.txt"
            source.write_text("Observed process metadata only; not a coverage denominator\n" + json.dumps(self.snapshot(module_status="access_denied", module_error=5)) + "\n", encoding="utf-8")
            events, summary = report.read_logs([source])
            result = report.build_report(events, input_summary=summary)
            self.assertEqual(len(result["process_observations"]), 1)
            self.assertEqual(summary["non_diagnostic_lines"], 1)
            self.assertNotIn("private-export", json.dumps(result))
            self.assertIsNone(result["metrics"]["entry_coverage"])


class CollectionSummaryTests(unittest.TestCase):
    def test_same_request_reason_uses_numeric_sequence_across_rotated_files(self):
        begin = event("dialog_show_begin")
        begin["seq"] = 1
        newer = event("contract_decision", reason_name="UnsupportedOptions")
        newer["seq"] = 20
        older = event("contract_decision", reason_name="ShellFilter")
        older["seq"] = 10
        end = event("dialog_show_terminal", terminal_status="native", fallback_reason="contract", hr=0)
        end["seq"] = 30
        result = report.build_report([newer, older, begin, end])
        self.assertEqual(result["requests"][0]["reason_code"], "UNSUPPORTED_OPTION_COMBINATION")
        for value in (begin, newer, older, end):
            value.pop("seq")
        original_order = report.build_report([begin, newer, older, end])
        self.assertEqual(original_order["requests"][0]["reason_code"], "UNSUPPORTED_CONTRACT_FEATURE")

    def test_collections_are_separate_from_requests_and_reasons(self):
        events = [
            {"event": "process_snapshot_collection", "data": {"scope": "visible_top_level_window_processes", "snapshot_count": 4, "enumeration_error": 0, "limit_reached": False}},
            {"event": "dialog_log_collection", "scope": "direct_whitelisted_log_excerpt", "incomplete": False, "files_read": 1, "records_emitted": 0},
            {"event": "registration_snapshot", "data": {"scope": "current_collector_context", "process_arch": 64, "registration": "conflict", "component": "present", "registry_error": 0, "component_error": 0}},
            {"event": "managed_log_sources", "data": {"collected": 2, "limit_reached": False, "unavailable_sources": 0, "last_status_error": 0}},
        ]
        result = report.build_report(events)
        self.assertEqual(len(result["collection_summaries"]), 4)
        self.assertEqual(result["unattributed_reason_counts"], {})
        self.assertEqual(result["requests"], [])
        self.assertIsNone(result["metrics"]["entry_coverage"])
        self.assertEqual(result["metrics"]["observed_requests"], 0)
        self.assertTrue(all(not row["counts_as_observed_request"] for row in result["collection_summaries"]))

    def test_top_level_read_errors_limits_and_tail_counts_are_preserved(self):
        summary = {"event": "dialog_log_collection", "scope": "direct_whitelisted_log_excerpt",
                   "incomplete": True, "directory_error": 5, "last_read_error": 299,
                   "files_read": 2, "tail_files": 1, "partial_lines_discarded": 2,
                   "output_limited": True, "max_records": 256}
        result = report.build_report([summary])
        row = result["collection_summaries"][0]
        for key in ("directory_error", "last_read_error", "files_read", "tail_files", "partial_lines_discarded", "output_limited", "max_records"):
            self.assertEqual(row[key], summary[key])
        self.assertTrue(result["metrics"]["diagnostic_evidence_incomplete"])
        self.assertEqual(result["requests"], [])

    def test_log_loss_prevents_missing_entry_claim_with_controlled_inventory(self):
        for summary in (
            {"event": "dialog_log_collection", "incomplete": True},
            {"event": "dialog_log_collection", "incomplete": False, "last_read_error": 5},
            {"event": "dialog_log_collection", "tail_files": 1},
            {"event": "managed_log_sources", "data": {"limit_reached": True}},
            {"event": "managed_log_sources", "data": {"unavailable_sources": 1, "last_status_error": 5}},
        ):
            result = report.build_report([summary], expected())
            self.assertIsNone(result["metrics"]["entry_coverage"])
            self.assertEqual(result["requests"][0]["reason_code"], "ENTRY_OBSERVATION_INCOMPLETE")
            self.assertEqual(result["metrics"]["entry_not_instrumented"], 0)

    def test_process_and_registration_collection_errors_do_not_taint_show_coverage(self):
        summaries = [
            {"event": "process_snapshot_collection", "data": {"enumeration_error": 5, "cancelled": True, "budget_exhausted": True}},
            {"event": "registration_snapshot", "data": {"scope": "current_collector_context", "registration": "read_error", "registry_error": 5, "component_error": 5}},
        ]
        result = report.build_report(summaries + [event("dialog_show_begin")], expected())
        self.assertEqual(result["metrics"]["entry_coverage"], 1)
        self.assertFalse(result["metrics"]["diagnostic_evidence_incomplete"])
        self.assertEqual(len(result["collection_summaries"]), 2)

    def test_collector_metadata_cannot_attach_to_a_matching_request(self):
        summary = event("registration_snapshot", scope="current_collector_context", conflict=True,
                        reason_code="REGISTRATION_MISMATCH", process_arch=32)
        result = report.build_report([event("dialog_show_begin", process_arch=64), summary,
                                     event("dialog_show_terminal", terminal_status="native", hr=0)])
        self.assertEqual(result["requests"][0]["process_arch"], "x64")
        self.assertEqual(result["requests"][0]["reason_code"], "NATIVE_FALLBACK_REASON_NOT_OBSERVED")
        self.assertNotIn("REGISTRATION_MISMATCH", result["requests"][0]["reasons_by_layer"].get("deployment", []))

    def test_excerpt_wrapper_keeps_original_correlation_and_legacy_registration_reason(self):
        begin = event("dialog_show_begin", process_arch=64)
        end = event("dialog_show_terminal", terminal_status="cancelled")
        for index, value in enumerate((begin, end), 1):
            value.update(kind="dialog_log_excerpt", source="dialog_auto", seq=index)
        registration = {"kind": "dialog_log_excerpt", "source": "dialog_auto", "event": "registration_snapshot", "session": 70, "pid": 42, "seq": 3, "data": {"conflict": True}}
        result = report.build_report([begin, end, registration])
        self.assertEqual(result["metrics"]["pulse_cancelled"], 1)
        self.assertEqual(result["metrics"]["observed_requests"], 1)
        self.assertEqual(result["collection_summaries"], [])
        self.assertEqual(result["unattributed_reason_counts"]["REGISTRATION_MISMATCH"], 1)

    def test_collection_whitelist_drops_raw_text_and_invalid_types(self):
        secret = r"C:\private\secret-document.txt"
        summary = {"event": "dialog_log_collection", "scope": secret, "directory_error": secret,
                   "files_read": True, "tail_files": -1, "output_limited": 1, "path": secret,
                   "reason_code": "REGISTRATION_MISMATCH", "incomplete": False}
        result = report.build_report([summary])
        row = result["collection_summaries"][0]
        for key in ("directory_error", "files_read", "tail_files", "output_limited"):
            self.assertIsNone(row[key])
        self.assertEqual(row["scope"], "unknown")
        self.assertNotIn("secret-document", json.dumps(result))
        self.assertEqual(result["unattributed_reason_counts"], {})


if __name__ == "__main__":
    unittest.main()
