import unittest
from summarize_explorer_handoff import summarize


class HandoffSummaryTest(unittest.TestCase):
    def event(self, seq, event, data, session=1):
        return dict(session=session, pid=10, seq=seq, event=event, data=data)

    def test_export_duplicates_do_not_inflate_fallback(self):
        event = self.event(1, "win_e_delivery", dict(id_high=2, id_low=3, state=3, native_fallback=1, error=0))
        result = summarize([event, event])
        self.assertEqual(result["duplicate_export_records_ignored"], 1)
        self.assertEqual(result["win_e"][0]["native_attempts"], 1)
        self.assertFalse(result["win_e"][0]["duplicate_or_conflicting_delivery"])

    def test_close_is_not_presentation_and_sessions_do_not_merge(self):
        result = summarize([
            self.event(1, "explorer_handoff", dict(request_id=7, phase=6)),
            self.event(1, "explorer_handoff", dict(request_id=7, phase=13), session=2),
        ])
        self.assertEqual(result["handoff_outcomes"], dict(closed_without_presentation_evidence=1, presented=1))

    def test_dispatched_and_fallback_is_conflict(self):
        result = summarize([
            self.event(1, "win_e_delivery", dict(id_high=2, id_low=3, state=2, native_fallback=0)),
            self.event(2, "win_e_delivery", dict(id_high=2, id_low=3, state=3, native_fallback=1)),
        ])
        self.assertTrue(result["win_e"][0]["duplicate_or_conflicting_delivery"])


if __name__ == "__main__":
    unittest.main()
