#requires -Version 7.2
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
$version = (Get-Content version.txt -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid release version' }
$tag = "v$version"
$repository = 'jimmgreen/pulse'
$base = "https://github.com/$repository/releases/download/$tag"
$normal = "PulseSetup-$version.exe"
$win81 = "PulseSetup-$version-win81.exe"
foreach ($file in @($normal, $win81)) {
    if (-not (Test-Path -LiteralPath "dist/$file")) { throw "Missing installer: $file" }
}
if (-not $env:PULSE_UPDATE_PRIVATE_KEY) { throw 'Configure the PULSE_UPDATE_PRIVATE_KEY Actions secret first' }
$keyPath = Join-Path ([IO.Path]::GetTempPath()) ("pulse-sign-" + [guid]::NewGuid().ToString('N') + '.pem')
$publicKey = (Get-Content cmake/update-public-key.txt -Raw).Trim()
try {
    [IO.File]::WriteAllText($keyPath, $env:PULSE_UPDATE_PRIVATE_KEY)
    & "$PSScriptRoot/create_update_manifest.ps1" -Installer "dist/$normal" -DownloadPage "$base/$normal" `
        -PrivateKey $keyPath -ExpectedPublicKey $publicKey -MinimumWindowsBuild 10240 -Output 'dist/update-manifest.json'
    & "$PSScriptRoot/create_update_manifest.ps1" -Installer "dist/$win81" -DownloadPage "$base/$win81" `
        -PrivateKey $keyPath -ExpectedPublicKey $publicKey -MinimumWindowsBuild 9600 -Output 'dist/update-manifest-win81.json'
} finally {
    Remove-Item -LiteralPath $keyPath -ErrorAction SilentlyContinue
}
$changesPath = "docs/releases/$version.md"
$changes = if (Test-Path $changesPath) { Get-Content $changesPath -Raw } else { '修复问题并改进使用体验。' }
$notesPath = 'dist/release-notes.md'
@"
# Pulse $version

## 下载安装

请按你的 Windows 版本选择安装包，两者均为 **64 位（x64）**。

| 系统 | 安装包 |
| --- | --- |
| **Windows 10 / Windows 11** | [$normal]($base/$normal) |
| **Windows 8.1 兼容版** | [$win81]($base/$win81) |

两个版本功能一致。Windows 8.1 上不可用的系统视觉效果会使用兼容显示方式。

## 本次更新

$changes

## 客户端更新

Pulse 启动后会自动检查新版；点击更新提示或「设置 → 关于与诊断 → 下载安装」即可下载、校验并启动安装程序。安装时按 Windows 提示确认管理员权限。

使用 1.0.2 或更早版本的用户，请先从上方链接手动安装一次。自 1.0.3 起，普通版和 Windows 8.1 版分别接收适用的更新。

Assets 中的 update-manifest*.json 是客户端使用的更新清单，无需手动下载。
"@ | Set-Content -LiteralPath $notesPath -Encoding utf8
$existing = & gh release view $tag --repo $repository --json isDraft 2>$null
if ($LASTEXITCODE -eq 0) {
    if (-not ($existing | ConvertFrom-Json).isDraft) { throw 'This version is already published; increment version.txt' }
    & gh release edit $tag --repo $repository --title "Pulse $version" --notes-file $notesPath
} else {
    & gh release create $tag --repo $repository --verify-tag --draft --title "Pulse $version" --notes-file $notesPath
}
if ($LASTEXITCODE -ne 0) { throw 'Could not prepare the release draft' }
& gh release upload $tag --repo $repository "dist/$normal" "dist/$win81" `
    dist/update-manifest.json dist/update-manifest-win81.json --clobber
if ($LASTEXITCODE -ne 0) { throw 'Could not upload release assets; release remains a draft' }
& gh release edit $tag --repo $repository --draft=false --latest
if ($LASTEXITCODE -ne 0) { throw 'Could not publish the release' }
