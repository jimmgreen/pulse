"""Run targeted menu checks with isolated diagnostic switches and log folders."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
build = root / "build"
with tempfile.TemporaryDirectory(prefix="pulse-context-diagnostics-") as temp:
    base = Path(temp)
    for enabled in (False, True):
        case = base / ("enabled" if enabled else "disabled")
        case.mkdir()
        local = case / "local"
        local.mkdir()
        env = dict(os.environ, LOCALAPPDATA=str(local))
        if enabled:
            (case / "pulse-context-diagnostics.flag").touch()
        for target, args in (("pulse_ctx_menu_controller_test", []),
                             ("pulse_shell_worker_lifecycle_test", ["--session-close"])):
            shutil.copy2(build / (target + ".exe"), case)
            result = subprocess.run([str(case / (target + ".exe")), *args],
                                    env=env, capture_output=True, timeout=120)
            (build / f"ctx-diag-{enabled}-{target}.log").write_bytes(result.stdout + result.stderr)
            assert result.returncode == 0, (target, enabled, result.returncode)
        ui = local / "Pulse/pulse_context_ui.log"
        host = local / "Pulse/pulse_shell_host.log"
        if enabled:
            text = ui.read_text(encoding="utf-8")
            if ui.with_suffix(".log.1").exists():
                text += ui.with_suffix(".log.1").read_text(encoding="utf-8")
            (build / "ctx-diag-ui-evidence.log").write_text(text, encoding="utf-8")
            for expected in ("stage=preferences", "drop-", "candidate-enabled", "committed-enabled"):
                assert expected in text, expected
            assert "ctxdiag sid=" in host.read_text(encoding="utf-8-sig")
            ui.write_bytes(b"x" * (1024 * 1024 + 1))
            result = subprocess.run([str(case / "pulse_ctx_menu_controller_test.exe")],
                                    env=env, capture_output=True, timeout=120)
            assert result.returncode == 0 and ui.with_suffix(".log.1").exists()
            print("PASS enabled: filter/final/session logs and 1 MiB rotation")
        else:
            assert not ui.exists()
            assert not host.exists() or "ctxdiag sid=" not in host.read_text(encoding="utf-8-sig")
            print("PASS disabled: no extra diagnostic logs")
        diagnostic_local = case / "encoding"
        diagnostic_local.mkdir()
        result = subprocess.run([str(case / "pulse_shell_worker_lifecycle_test.exe"), "--diagnostic-log"],
                                env=dict(env, LOCALAPPDATA=str(diagnostic_local)), capture_output=True, timeout=30)
        assert result.returncode == 0
        lines = (diagnostic_local / "Pulse/pulse_shell_host.log").read_text(encoding="utf-8").splitlines()
        assert len(lines) == (81 if enabled else 1)
        assert sum("中文菜单诊断" in line for line in lines) == 1
        if enabled:
            for sid in range(9000, 9004):
                assert sum(f"ctxdiag sid={sid} " in line for line in lines) == 20
        print(f"PASS host UTF-8/concurrent logging enabled={enabled}")
print("PASS isolated fixtures cleaned")

