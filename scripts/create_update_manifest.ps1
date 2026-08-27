#requires -Version 7.2
param(
    [Parameter(Mandatory = $true)]
    [string]$Installer,
    [Parameter(Mandatory = $true)]
    [uri]$DownloadPage,
    [Parameter(Mandatory = $true)]
    [string]$PrivateKey,
    [uint32]$MinimumWindowsBuild = 19045,
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $repo "version.txt") -TotalCount 1).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw "version.txt must contain a three-part numeric version"
}
if ($DownloadPage.Scheme -ne "https") {
    throw "DownloadPage must use HTTPS"
}
if ($MinimumWindowsBuild -lt 10240) {
    throw "MinimumWindowsBuild must identify a Windows 10 or newer build"
}

$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$privateKeyPath = (Resolve-Path -LiteralPath $PrivateKey).Path
if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path (Split-Path -Parent $installerPath) "update-manifest.json"
}
$outputPath = [System.IO.Path]::GetFullPath($Output)
$hash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
$download = $DownloadPage.AbsoluteUri
$payload = "schema=1`nversion=$version`nminimum_windows_build=$MinimumWindowsBuild`n" +
           "download_page=$download`ninstaller_sha256=$hash`n"

$ecdsa = [System.Security.Cryptography.ECDsa]::Create()
try {
    $ecdsa.ImportFromPem([System.IO.File]::ReadAllText($privateKeyPath))
    if ($ecdsa.KeySize -ne 256) {
        throw "PrivateKey must be an ECDSA P-256 PEM key"
    }
    $signature = $ecdsa.SignData(
        [System.Text.Encoding]::UTF8.GetBytes($payload),
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
    if ($signature.Length -ne 64) {
        throw "Unexpected ECDSA signature size"
    }
    $parameters = $ecdsa.ExportParameters($false)
    $public = [byte[]]::new(65)
    $public[0] = 4
    [Array]::Copy($parameters.Q.X, 0, $public, 1, 32)
    [Array]::Copy($parameters.Q.Y, 0, $public, 33, 32)

    $manifest = [ordered]@{
        schema = 1
        version = $version
        minimum_windows_build = $MinimumWindowsBuild
        download_page = $download
        installer_sha256 = $hash
        signature = [Convert]::ToBase64String($signature)
    } | ConvertTo-Json
    [System.IO.File]::WriteAllText(
        $outputPath, $manifest + "`n", [System.Text.UTF8Encoding]::new($false))
    Write-Host "Update manifest written to $outputPath"
    Write-Host "PULSE_UPDATE_PUBLIC_KEY_HEX=$([Convert]::ToHexString($public).ToLowerInvariant())"
} finally {
    $ecdsa.Dispose()
}
