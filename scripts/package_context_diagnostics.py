"""Package the existing Release payload with opt-in context menu diagnostics."""
from pathlib import Path
import hashlib
import importlib.util
import json
import zipfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("pack_setup", root / "tools/pack_installer/pack_installer.py")
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)
version = (root / "version.txt").read_text().strip()
name = f"Pulse-{version}-ContextDiagnostics-20261030"
out = root / "dist" / (name + ".zip")
files = pack.collect(root, root / "build")
readme = """Pulse 右键诊断版（1.0.58，2026-10-30）

1. 完全退出原来的 Pulse（包括托盘），将压缩包全部解压到新的文件夹。
2. 双击此文件夹的 pulse.exe，使用与原来相同的权限运行，不必额外以管理员运行。
3. 对发生缺项的同一个文件/文件夹右键，保持菜单打开约 5 秒。
   拍下 Pulse 和 Windows 资源管理器的右键对照，说明缺少哪些项、是否按 Shift。
4. 退出诊断版，双击 Collect-Logs.cmd。将生成的 Collected-Logs 文件夹压缩回传，
   附复现时间、Windows 版本、对照截图和具体缺少的菜单名称。

诊断默认启用：程序旁的 pulse-context-diagnostics.flag 是开关。
删除该标记并重启程序即可关闭额外日志。不需要修改注册表或禁用右键扩展。
本包仍使用原有 Pulse 设置，便于复现隐藏项问题；请勿先重置右键设置。
日志可能含文件路径、程序名称和菜单文字，请检查后私下发送，不要公开发布。
每种日志自动轮转；收集工具只复制指定日志，不上传、不修改原日志。
诊断版未在反馈用户的 Windows 10 现场验证，目的是收集证据，并非已修复缺项。
"""
collector = r'''@echo off
setlocal
set "SRC=%LOCALAPPDATA%\Pulse"
set "DEST=%~dp0Collected-Logs"
if not exist "%DEST%" mkdir "%DEST%"
for %%F in (pulse_context_ui.log pulse_context_ui.log.1 pulse_shell_host.log pulse_shell_host.log.1) do (
  if exist "%SRC%\%%F" copy /y "%SRC%\%%F" "%DEST%\%%F" >nul
)
ver > "%DEST%\windows-version.txt"
echo Collected: %DATE% %TIME%>> "%DEST%\windows-version.txt"
copy /y "%~dp0diagnostic-manifest.json" "%DEST%\diagnostic-manifest.json" >nul
echo Logs copied to: "%DEST%"
echo If no logs exist, fully exit Pulse and run pulse.exe from this folder again.
pause
'''
manifest = {"version": version, "build": name, "files": [
    {"path": dest, "sha256": hashlib.sha256(src.read_bytes()).hexdigest()}
    for src, dest in files]}
out.parent.mkdir(exist_ok=True)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
    for src, dest in files:
        archive.write(src, name + "/" + dest)
    for dest, data in {
        "README.txt": readme.encode("utf-8-sig"),
        "Collect-Logs.cmd": collector.replace("\n", "\r\n").encode("ascii"),
        "pulse-context-diagnostics.flag": b"context-diagnostics-v1\r\n",
        "diagnostic-manifest.json": json.dumps(manifest, indent=2).encode("utf-8"),
    }.items():
        archive.writestr(name + "/" + dest, data)
with zipfile.ZipFile(out) as archive:
    assert archive.testzip() is None
    for entry in manifest["files"]:
        assert hashlib.sha256(archive.read(name + "/" + entry["path"])).hexdigest() == entry["sha256"]
print(f"Verified {len(files)} payload files: {out} ({out.stat().st_size} bytes)")
