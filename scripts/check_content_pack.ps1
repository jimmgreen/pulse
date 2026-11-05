# Checks an enhancement pack's assets in -PackDir against their
# <key>-pack-assets.json (written with the catalog by pulse_pack_catalog):
# this version, exactly the pack executables, every LZMS asset with the pinned
# size and SHA-256. With -BuildDir, the executables there must be the very
# files the catalog pins - signing after the catalog was generated fails here.
# -Key content (default): content search; -Key ai: the AI pack.
param(
    [Parameter(Mandatory = $true)]
    [string]$PackDir,
    [string]$BuildDir,
    [string]$Repository,
    [ValidateSet('content', 'ai')]
    [string]$Key = 'content'
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Repository)) {
    if ($env:GITHUB_REPOSITORY) { $Repository = $env:GITHUB_REPOSITORY }
    else { $Repository = 'fei98625/pulse' }
}
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
    throw 'Repository must be a GitHub owner/repository name'
}
$repo = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot/release_payload.ps1"
$version = (Get-Content -LiteralPath (Join-Path $repo 'version.txt') -TotalCount 1).Trim()
$label = if ($Key -eq 'ai') { 'AI pack' } else { 'Content pack' }
$executables = if ($Key -eq 'ai') { $PulseAiPackExecutables } else { $PulseContentPackExecutables }
$manifestPath = Join-Path $PackDir "$Key-pack-assets.json"
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "Missing $label manifest: $manifestPath" }
$pack = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($pack.key -ne $Key) { throw "Unexpected $label key: $($pack.key)" }
if ($pack.version -ne $version) { throw "$label is version $($pack.version), expected $version" }
$expectedBaseUrl = "https://github.com/$Repository/releases/download/v$version/"
if ($pack.base_url -ne $expectedBaseUrl) {
    throw "$label points at another release: $($pack.base_url)"
}
$names = @($pack.files | ForEach-Object { $_.name } | Sort-Object)
$expected = @($executables | Sort-Object)
if (($names -join '|') -ne ($expected -join '|')) { throw "$label files differ: $($names -join ', ')" }

function Get-Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }

foreach ($file in $pack.files) {
    if ($file.asset -ne "$Key-$($pack.channel)-$($file.name).lzms") { throw "Unexpected asset name: $($file.asset)" }
    $asset = Join-Path $PackDir $file.asset
    if (-not (Test-Path -LiteralPath $asset)) { throw "Missing $label asset: $asset" }
    $item = Get-Item -LiteralPath $asset
    if ($item.Length -ne [int64]$file.packed_size -or (Get-Sha256 $asset) -ne $file.packed_sha256) {
        throw "$label asset does not match its catalog: $($file.asset)"
    }
    if ($BuildDir) {
        $binary = Join-Path $BuildDir $file.name
        if (-not (Test-Path -LiteralPath $binary)) { throw "Missing $label binary: $binary" }
        if ((Get-Item -LiteralPath $binary).Length -ne [int64]$file.size -or (Get-Sha256 $binary) -ne $file.sha256) {
            throw "$($file.name) changed after its catalog was generated; rebuild the pulse target"
        }
    }
}
$bytes = ($pack.files | Measure-Object -Property packed_size -Sum).Sum
Write-Output "$label check passed: $($pack.channel) $version, $($pack.files.Count) files, $bytes download bytes."