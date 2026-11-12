import unittest
from test_report import report

class LaunchPreflightTests(unittest.TestCase):
    def make(self, **data):
        event = {"schema": 1, "session": "{12345678-1234-1234-1234-123456789ABC}", "pid": 42,
                 "event": "target_preflight", "data": data}
        return report.build_report([event, event])
    def test_existing_is_warning_not_success(self):
        r = self.make(existing_target_count=1, scan_complete=1, query_errors=0, warning="existing_target", path="PRIVATE", certified=1)
        self.assertEqual(r["requests"], [])
        self.assertIsNone(r["metrics"]["entry_coverage"])
        self.assertEqual(r["launch_preflights"][0]["warning"], "existing_target")
        self.assertFalse(r["launch_preflights"][0]["certified"])
        self.assertNotIn("PRIVATE", str(r))
        self.assertEqual(len(r["launch_preflights"]), 1)
    def test_unknown_is_not_no_existing(self):
        for data in ({"existing_target_count":0,"scan_complete":0}, {"existing_target_count":0,"scan_complete":1,"query_errors":1}, {"existing_target_count":0,"scan_complete":1,"query_errors":0,"timed_out":1}, {}):
            r = self.make(**data)
            self.assertEqual(r["launch_preflights"][0]["warning"], "scan_incomplete")
    def test_zero_only_with_complete(self):
        r = self.make(existing_target_count=0, scan_complete=1, query_errors=0, cancelled=0, vanished_processes=0)
        self.assertEqual(r["launch_preflights"][0]["warning"], "none")
        self.assertFalse(r["launch_preflights"][0]["counts_as_observed_request"])
