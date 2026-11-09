"""Verify a real --test-instance diagnostic export, including the ZIP payload."""
import json, re, sys, zipfile, hashlib
from pathlib import Path
root = Path(sys.argv[1])
archives = sorted(root.glob("export/**/*.zip"), key=lambda p: p.stat().st_mtime)
assert archives, "export ZIP missing"
with zipfile.ZipFile(archives[-1]) as archive:
    names = archive.namelist()
    events = []
    for name in names:
        if "/Runtime/app-" in name and name.endswith(".jsonl") and ".critical." not in name:
            events += [json.loads(line) for line in archive.read(name).decode("utf-8-sig").splitlines() if line]
    slow = {e["data"]["frame_id"]:e["data"] for e in events if e["event"] == "slow_frame"}
    assert slow, "slow frame event missing"
    for event in events:
        if event["event"] == "slow_frame_cpu" and event["data"]["frame_id"] in slow:
            slow[event["data"]["frame_id"]].update(event["data"])
    for event in ["slow_frame_detail", "slow_frame_luma", "slow_frame_luma_reasons", "slow_frame_render", "slow_frame_rows"]:
        assert any(e["event"] == event and e["data"]["frame_id"] in slow for e in events), event
    stacks = "".join(archive.read(n).decode("utf-8-sig") for n in names if "/Perf/frame-" in n and n.endswith(".log"))
    captured = {int(i) for i in re.findall(r"frame_id=(\d+).*status=captured", stacks)}
    joined = captured.intersection(slow)
    assert joined and "+0x" in stacks, "no correlated stack"
    assert any(slow[i]["ui_cpu_valid"] and slow[i]["frame_us"] - slow[i]["ui_cpu_us"] > 80000 for i in joined), "injected wait not distinguished from CPU"
    luma = [e["data"] for e in events if e["event"] == "slow_frame_luma"]
    assert any(d["luma_layout_us"] or d["luma_draw_us"] for d in luma), "diagnostic timings disabled"
    envs = [n for n in names if "/Perf/perf-environment-" in n and n.endswith(".json")]
    assert envs, "environment missing"
    env = json.loads(archive.read(envs[-1]))
    assert env["font_snapshot_available"], "font snapshot missing"
    exe = next(m for m in env["modules"] if m["name"] == "process_executable")
    assert exe["sha256"].lower() == hashlib.sha256(Path(exe["path"]).read_bytes()).hexdigest(), "binary hash mismatch"
    assert env["adapters"] or env["adapter_enumeration_hr"] != 0, "GPU evidence missing"
    active = [e["data"] for e in events if e["event"] == "slow_frame_adapter" and e["data"]["frame_id"] in slow]
    assert active, "active adapter event missing"
    assert any(d["valid"] and any(a["luid_low"] == d["luid_low"] and a["luid_high"] == d["luid_high"] for a in env["adapters"]) for d in active), "active adapter not matched to environment"
    print("PASS: ZIP contains correlated slow frame, stages, CPU, Luma reasons, module-RVA stack, font, GPU and verified executable hash")
    print("archive=" + str(archives[-1]))
    print("joined_frame_ids=" + str(sorted(joined)))
