#requires -Version 7.2
param(
    [Parameter(Mandatory = $true)]
    [string]$CodeSigningThumbprint,
    [Parameter(Mandatory = $true)]
    [uri]$TimestampUrl,
    [Parameter(Mandatory = $true)]
    [uri]$DownloadPage,
    [Parameter(Mandatory = $true)]
    [string]$ManifestPrivateKey,
    [uint32]$MinimumWindowsBuild = 19045
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $repo "version.txt") -TotalCount 1).Trim()
$buildPath = [System.IO.Path]::GetFullPath((Join-Path $repo "build"))
$privateKeyPath = (Resolve-Path -LiteralPath $ManifestPrivateKey).Path
if ($TimestampUrl.Scheme -ne "https" -or $DownloadPage.Scheme -ne "https") {
    throw "TimestampUrl and DownloadPage must use HTTPS"
}
if ($CodeSigningThumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
    throw "CodeSigningThumbprint must be a SHA-1 certificate thumbprint"
}

& (Join-Path $repo "build_release.bat")
if ($LASTEXITCODE -ne 0) { throw "Release build failed" }

$signTool = (Get-Command signtool.exe -ErrorAction Stop).Source
$binaries = @("pulse.exe", "Pulse.Index.exe", "Pulse.Preview.exe", "pulse_shell.exe")
foreach ($name in $binaries) {
    $path = Join-Path $buildPath $name
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing release binary: $path" }
    & $signTool sign /sha1 $CodeSigningThumbprint /fd SHA256 /tr $TimestampUrl.AbsoluteUri /td SHA256 $path
    if ($LASTEXITCODE -ne 0) { throw "Code signing failed: $path" }
}

& (Join-Path $repo "build_installer.bat") /skipbuild
if ($LASTEXITCODE -ne 0) { throw "Installer build failed" }
$installer = Join-Path $repo "dist\PulseSetup-$version.exe"
& $signTool sign /sha1 $CodeSigningThumbprint /fd SHA256 /tr $TimestampUrl.AbsoluteUri /td SHA256 $installer
if ($LASTEXITCODE -ne 0) { throw "Installer signing failed" }

& (Join-Path $PSScriptRoot "archive_symbols.ps1") -BuildDir "build"
if ($LASTEXITCODE -ne 0) { throw "Symbol archive failed" }

$manifest = Join-Path $repo "dist\update-manifest.json"
& (Join-Path $PSScriptRoot "create_update_manifest.ps1") `
    -Installer $installer -DownloadPage $DownloadPage -PrivateKey $privateKeyPath `
    -MinimumWindowsBuild $MinimumWindowsBuild -Output $manifest
if ($LASTEXITCODE -ne 0) { throw "Update manifest signing failed" }

$buildId = (Get-Content -LiteralPath (Join-Path $buildPath "pulse_build_id.txt") -TotalCount 1).Trim()
$hashArtifacts = @($installer, $manifest)
foreach ($name in $binaries) { $hashArtifacts += Join-Path $buildPath $name }
$hashes = foreach ($path in $hashArtifacts) {
    $item = Get-Item -LiteralPath $path
    [ordered]@{
        file = $item.Name
        bytes = $item.Length
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$hashManifest = [ordered]@{
    schema = 1
    version = $version
    build_id = $buildId
    artifacts = @($hashes)
} | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText(
    (Join-Path $repo "dist\hashes.json"), $hashManifest + "`n",
    [System.Text.UTF8Encoding]::new($false))

Write-Host "Release package complete: dist\PulseSetup-$version.exe"
