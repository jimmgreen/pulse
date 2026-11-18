"""Summarize exported Pulse Runtime JSONL without treating observations as global coverage."""
import argparse
from collections import Counter
import json
from pathlib import Path


def summarize(records):
    seen = set()
    candidates, startup, bridge, launcher = Counter(), Counter(), Counter(), Counter()
    handoffs, keyboard = {}, {}
    duplicates = 0
    for record in records:
        identity = (record.get("session"), record.get("pid"))
        sequence = record.get("seq")
        if sequence is not None:
            key = identity + (sequence,)
            if key in seen:
                duplicates += 1
                continue
            seen.add(key)
        data = record.get("data", {})
        if not isinstance(data, dict):
            continue
        event = record.get("event")
        if event == "explorer_candidate":
            candidates[str(data.get("reason", "unknown"))] += 1
        elif event == "explorer_watcher_start":
            startup[f"phase={data.get('phase')},hr={data.get('hr')}"] += 1
        elif event == "shell_registry_bridge":
            bridge[f"phase={data.get('phase')},hr={data.get('hr')}"] += 1
        elif event == "shell_entry_launch_failed":
            launcher[f"phase={data.get('phase')},error={data.get('error')},"
                     f"native_attempted={data.get('native_attempted')},native_error={data.get('native_error')}"] += 1
        elif event == "explorer_handoff" and "request_id" in data:
            key = identity + (data["request_id"],)
            item = handoffs.setdefault(key, {"phases": [], "last_elapsed_ms": 0, "rejections": []})
            phase = data.get("phase")
            item["phases"].append(phase)
            item["last_elapsed_ms"] = max(item["last_elapsed_ms"], data.get("elapsed_ms", 0))
            if phase in (8, 9):
                item["rejections"].append({"phase": phase, "code": data.get("code"), "detail": data.get("detail")})
        elif event in ("win_e_chord", "win_e_delivery"):
            key = identity + (data.get("id_high"), data.get("id_low"))
            item = keyboard.setdefault(key, {"accepted": 0, "dispatched": 0, "cancelled": 0,
                                              "native_attempts": 0, "errors": []})
            if event == "win_e_chord":
                item["accepted"] += int(data.get("accepted") == 1)
            else:
                item["dispatched"] += int(data.get("state") == 2)
                item["cancelled"] += int(data.get("state") == 3)
                item["native_attempts"] += int(data.get("native_fallback") == 1)
                if data.get("error"):
                    item["errors"].append(data["error"])
    outcomes = Counter()
    for item in handoffs.values():
        phases = item["phases"]
        outcome = ("presented" if 13 in phases else "closed_without_presentation_evidence" if 6 in phases
                   else "rolled_back" if 11 in phases else "close_failed" if 8 in phases
                   else "cancelled_or_expired" if 7 in phases else "incomplete_evidence")
        item["outcome"] = outcome
        outcomes[outcome] += 1
    return {
        "scope": "Observed log events only; no global takeover percentage or proof of native window appearance.",
        "duplicate_export_records_ignored": duplicates,
        "candidate_reasons": dict(candidates), "watcher_start": dict(startup),
        "shell_registry_bridge": dict(bridge), "launcher_failures": dict(launcher),
        "handoff_outcomes": dict(outcomes),
        "handoffs": [{"identity": list(key), **item} for key, item in handoffs.items()],
        "win_e": [{"identity": list(key), **item,
                   "duplicate_or_conflicting_delivery": item["native_attempts"] > 1 or
                       (item["dispatched"] > 0 and item["native_attempts"] > 0)}
                  for key, item in keyboard.items()],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Runtime directory or one JSONL file")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if not args.source.exists():
        parser.error("source does not exist")
    paths = sorted(path for path in args.source.rglob("*") if path.is_file() and
                   path.name.endswith((".jsonl", ".jsonl.1"))) if args.source.is_dir() else [args.source]
    malformed = 0

    def records():
        nonlocal malformed
        for path in paths:
            with path.open(encoding="utf-8-sig", errors="replace") as stream:
                for line in stream:
                    try:
                        value = json.loads(line)
                        if not isinstance(value, dict):
                            raise ValueError("not an object")
                        yield value
                    except (ValueError, TypeError):
                        malformed += 1

    result = summarize(records())
    result.update(files_read=len(paths), malformed_records=malformed)
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
