"""Wrap an already verified automatic-dialog bundle; never enables integration."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'pack_dialog_experiment'))
import pack

README = r"""Pulse 自动打开/保存接入测试版（R43，x64）

先完整解压到固定的本地目录。启用后不要移动、删除或覆盖这个目录；先恢复原生再更新。
1. 完全退出旧 Pulse（包括托盘），运行 Start-Pulse.cmd。
2. 双击 Enable-Auto.cmd，直接启用本包组件；应显示 enabled=1、error=0。
   若显示旧组件包，请选择本包内 Bundle 文件夹。无需选择应用或逐个通过启动器打开。
3. 正常从桌面或开始菜单启动要测试的软件。已经运行的软件建议先保存工作再正常退出重开。
4. 自由测试打开、另存为、取消。发现问题请记录软件名、操作和时间。
5. 双击 Collect-Logs.cmd，回传生成的 Collected-Logs-时间 文件夹。
6. 测试结束在设置中停用，或双击 Restore-Native.cmd；该工具无需启动Pulse即可恢复原生。

范围：当前用户、64位、普通权限、标准 FileOpenDialog/FileSaveDialog 接口。
32位、管理员/受限进程、传统GetOpenFileName/GetSaveFileName或自绘窗口不在此次自动接入覆盖范围。
不支持的标准接口组合继续使用系统窗口，日志记录原因。看到系统窗口请如实报告，不等于所有入口已接入。
不改变文件内容，不发送文件。此包未开启全局接入；启用由你在设置中操作。

日志复用 %LOCALAPPDATA%\Pulse\Diagnostics\Runtime 下 dialog_auto*.jsonl 及轮转文件。
记录进程/线程/时间、契约结果、错误码和显示结果，不记录打开文件内容。
收集器只复制本地指定日志和版本清单，不上传。崩溃日志可能包含路径，回传前可查看。
系统入口（文件夹、此电脑、Win+E）仍由现有对应设置控制，自动对话框开关不替代这些选项。

"""
COLLECT = r"""$ErrorActionPreference = 'Stop'
$dest = Join-Path $PSScriptRoot ('Collected-Logs-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $dest -ErrorAction Stop | Out-Null
$reg = Get-ItemProperty 'HKCU:\Software\Pulse\DialogAuto' -ErrorAction SilentlyContinue
$reg | Select-Object Enabled,DllPath,Stage | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dest 'registered-component.json') -Encoding UTF8
if ($reg.DllPath -and (Test-Path -LiteralPath $reg.DllPath)) {
    Get-FileHash -LiteralPath $reg.DllPath -Algorithm SHA256 | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dest 'registered-component-hash.json') -Encoding UTF8
}
$root = Join-Path $env:LOCALAPPDATA 'Pulse' 
$runtime = Join-Path $root 'Diagnostics\Runtime'
if (Test-Path -LiteralPath $runtime) {
    Get-ChildItem -LiteralPath $runtime -File | Where-Object { $_.Name -like 'dialog_auto*.jsonl*' } | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $dest
    }
}
foreach ($name in @('pulse_crash.log', 'pulse_crash.log.1')) {
    $file = Join-Path $root $name
    if (Test-Path -LiteralPath $file) { Copy-Item -LiteralPath $file -Destination $dest }
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Bundle\manifest.json') -Destination (Join-Path $dest 'build-manifest.json')
& (Join-Path $PSScriptRoot 'Bundle\PulseDialogAutoControl.exe') --status | Set-Content -LiteralPath (Join-Path $dest 'integration-status.txt') -Encoding UTF8
('Collected: ' + (Get-Date -Format o)) | Set-Content -LiteralPath (Join-Path $dest 'collection-time.txt') -Encoding UTF8
Write-Host ('Logs copied to: ' + $dest)


"""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bundle', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    manifest = pack.verify(args.bundle)
    required = {'PulseDialogAuto64.dll', 'PulseDialogAutoControl.exe'}
    if not required <= {f['path'] for f in manifest['files']}:
        raise ValueError('bundle lacks automatic entry')
    if args.output.exists() or Path(str(args.output) + '.zip').exists():
        raise ValueError('output already exists')
    args.output.mkdir(parents=True)
    shutil.copytree(args.bundle, args.output / 'Bundle')
    pack.verify(args.output / 'Bundle')
    files = {'Enable-Auto.cmd': b'@echo off\r\n"%~dp0Bundle\\PulseDialogAutoControl.exe" --enable "%~dp0Bundle"\r\n"%~dp0Bundle\\PulseDialogAutoControl.exe" --status\r\npause\r\n', 'README.txt': README.encode('utf-8-sig'), 'Collect-Logs.ps1': COLLECT.encode('utf-8-sig'),
        'Start-Pulse.cmd': b'@echo off\r\nstart "" "%~dp0Bundle\\pulse.exe"\r\n',
        'Restore-Native.cmd': b'@echo off\r\n"%~dp0Bundle\\PulseDialogAutoControl.exe" --disable\r\npause\r\n',
        'Collect-Logs.cmd': b'@echo off\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Collect-Logs.ps1"\r\npause\r\n',
    }
    for name, data in files.items(): (args.output / name).write_bytes(data)
    archive = Path(str(args.output) + '.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        for p in sorted(args.output.rglob('*')):
            if p.is_file(): z.write(p, args.output.name + '/' + p.relative_to(args.output).as_posix())
    with zipfile.ZipFile(archive) as z:
        if z.testzip() is not None: raise ValueError('ZIP corrupt')
        for p in args.output.rglob('*'):
            if p.is_file():
                assert z.read(args.output.name + '/' + p.relative_to(args.output).as_posix()) == p.read_bytes()
    print(json.dumps({'archive':str(archive), 'bytes':archive.stat().st_size,
        'sha256':hashlib.sha256(archive.read_bytes()).hexdigest(), 'bundle_files':len(manifest['files'])}))
if __name__ == '__main__': main()
