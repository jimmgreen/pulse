param(
    [Parameter(Mandatory)][string]$AssetDirectory,
    [Parameter(Mandatory)][string]$Installer,
    [Parameter(Mandatory)][string]$Commit,
    [Parameter(Mandatory)][string]$Notes,
    [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
$repository = 'ClauxQz4/pulse'
$directory = (Resolve-Path -LiteralPath $AssetDirectory).Path
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$notesPath = (Resolve-Path -LiteralPath $Notes).Path
$manifest = Get-Content -LiteralPath (Join-Path $directory 'preview-packs-assets.json') -Raw | ConvertFrom-Json
$tag = $manifest.release_tag
if ($tag -notmatch '^preview-packs-[A-Za-z0-9_.-]+$') { throw 'Expected a preview-packs release tag' }
if ($Commit -notmatch '^[a-fA-F0-9]{40}$') { throw 'Provide the exact source commit' }
if ([IO.Path]::GetExtension($installerPath) -ne '.exe') { throw 'Expected an installer executable' }
$assets = @($installerPath)
$seen = @{}
foreach ($asset in $manifest.assets) {
    if ($asset.name -notmatch '^[A-Za-z0-9_.-]+$' -or $seen.ContainsKey($asset.name)) { throw 'Invalid or duplicate asset name' }
    $seen[$asset.name] = $true
    $path = Join-Path $directory $asset.name
    if ((Get-Item -LiteralPath $path).Length -ne $asset.size -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $asset.sha256) {
        throw "Asset size/hash mismatch: $($asset.name)"
    }
    $assets += $path
}
foreach ($key in 'ffmpeg', 'images', 'raw', 'archive') {
    $packManifests = @(Get-ChildItem -LiteralPath $directory -Filter "pulse-pack-$key-*.json" -File)
    if ($packManifests.Count -ne 1) { throw "Expected one manifest for $key" }
    $pack = Get-Content -LiteralPath $packManifests[0].FullName -Raw | ConvertFrom-Json
    if ($pack.id -ne $key -or $pack.release_tag -ne $tag -or @($pack.files).Count -eq 0) { throw "Invalid manifest for $key" }
    foreach ($file in $pack.files) {
        $match = @($manifest.assets | Where-Object { $_.name -eq $file.download_name })
        if ($match.Count -ne 1 -or $match[0].size -ne $file.packed_size -or $match[0].sha256 -ne $file.packed_sha256) {
            throw "Manifest references an unverified asset: $($file.name)"
        }
    }
    $assets += $packManifests[0].FullName
}
$assets += Join-Path $directory 'preview-packs-assets.json'
Write-Host "Verified $($assets.Count) release files for $tag"
if ($ValidateOnly) { return }
# Published assets are immutable: use a new tag for any corrected build.
$existing = & gh release view $tag --repo $repository --json isDraft 2>$null
if ($LASTEXITCODE -eq 0) {
    if (-not ($existing | ConvertFrom-Json).isDraft) { throw 'Release is already public; choose a new tag' }
} else {
    & gh release create $tag --repo $repository --target $Commit --draft --prerelease --latest=false `
        --title 'Pulse preview packs test' --notes-file $notesPath
    if ($LASTEXITCODE -ne 0) { throw 'Could not create draft release' }
}
& gh release upload $tag --repo $repository @assets --clobber
if ($LASTEXITCODE -ne 0) { throw 'Upload failed; release remains a draft' }
& gh release edit $tag --repo $repository --draft=false --prerelease --latest=false --notes-file $notesPath
if ($LASTEXITCODE -ne 0) { throw 'Could not publish preview release' }
