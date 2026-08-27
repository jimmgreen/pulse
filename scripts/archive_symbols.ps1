param(
    [string]$BuildDir = "build",
    [string]$OutputRoot = "dist\symbols"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $repo "version.txt") -TotalCount 1).Trim()
$buildPath = [System.IO.Path]::GetFullPath((Join-Path $repo $BuildDir))
$buildIdPath = Join-Path $buildPath "pulse_build_id.txt"
if (-not (Test-Path -LiteralPath $buildIdPath)) {
    throw "Missing build id: $buildIdPath"
}
$buildId = (Get-Content -LiteralPath $buildIdPath -TotalCount 1).Trim()
$destination = [System.IO.Path]::GetFullPath((Join-Path $repo (Join-Path $OutputRoot (Join-Path $version $buildId))))
New-Item -ItemType Directory -Path $destination -Force | Out-Null

$artifacts = @(
    "pulse.exe", "pulse.pdb",
    "Pulse.Index.exe", "Pulse.Index.pdb",
    "Pulse.Preview.exe", "Pulse.Preview.pdb",
    "pulse_shell.exe", "pulse_shell.pdb"
)
$manifest = @()
foreach ($name in $artifacts) {
    $source = Join-Path $buildPath $name
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Missing symbol artifact: $source"
    }
    Copy-Item -LiteralPath $source -Destination $destination -Force
    $hash = Get-FileHash -LiteralPath $source -Algorithm SHA256
    $item = Get-Item -LiteralPath $source
    $manifest += [ordered]@{
        file = $name
        bytes = $item.Length
        sha256 = $hash.Hash.ToLowerInvariant()
    }
}

[ordered]@{
    schema = 1
    version = $version
    build_id = $buildId
    artifacts = $manifest
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $destination "manifest.json") -Encoding utf8

Write-Host "Symbols archived to $destination"
