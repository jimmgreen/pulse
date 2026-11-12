param(
    [Parameter(Mandatory=$true)][string]$LumaTextSource,
    [Parameter(Mandatory=$true)][string]$HarfBuzzSource,
    [Parameter(Mandatory=$true)][string]$FreeTypeSource,
    [Parameter(Mandatory=$true)][string]$PdfiumRoot,
    [string]$Build = "build-dialog-x86-next",
    [string]$VcVars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$buildPath = [IO.Path]::GetFullPath((Join-Path $repo $Build))
if (-not $buildPath.StartsWith($repo + "\", [StringComparison]::OrdinalIgnoreCase)) { throw "Build must stay inside the workspace" }
$copy = Join-Path $buildPath "_src\lumatext"
$sdk = Get-Content -LiteralPath (Join-Path $repo "third_party\lumatext\sdk-manifest.json") -Raw | ConvertFrom-Json
$shortCommit = $sdk.source_base_commit.Substring(0, 7)
$archive = Join-Path $buildPath ("lumatext-source-" + $shortCommit + ".tar")
$upstream = Join-Path $buildPath ("_upstream\lumatext-" + $shortCommit)
if (-not (Test-Path -LiteralPath $copy)) {
    New-Item -ItemType Directory -Path $buildPath -Force | Out-Null
    if ((Test-Path -LiteralPath $archive) -or (Test-Path -LiteralPath $upstream)) { throw "Use a new build directory after incomplete source preparation" }
    & git -c "safe.directory=$LumaTextSource" -C $LumaTextSource archive --format=tar "--output=$archive" $sdk.source_base_commit
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $sdk.source_snapshot_sha256) { throw "LumaText source archive differs from the pinned x64 SDK" }
    New-Item -ItemType Directory -Path $upstream -Force | Out-Null
    & tar -xf $archive -C $upstream
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    python -B (Join-Path $PSScriptRoot "prepare_lumatext_x86.py") --source $upstream --output $copy
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sdk.source_snapshot_sha256) {
    throw "LumaText source archive hash changed"
}
$provenance = Get-Content -LiteralPath (Join-Path $copy "x86-source.json") -Raw | ConvertFrom-Json
foreach ($file in $provenance.files) {
    if ((Get-FileHash -LiteralPath (Join-Path $copy $file.path) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.copy_sha256) {
        throw "Adapted LumaText source changed: $($file.path)"
    }
}
$arguments = @("-S", $repo, "-B", $buildPath, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
    "-DPULSE_WITH_SELFTEST=OFF", "-DPULSE_WITH_TEST_HOOKS=OFF", "-DPULSE_WITH_LUMATEXT=ON",
    "-DLUMATEXT_SOURCE_DIR=$copy", "-DFETCHCONTENT_SOURCE_DIR_HARFBUZZ=$HarfBuzzSource",
    "-DFETCHCONTENT_SOURCE_DIR_FREETYPE=$FreeTypeSource", "-DPULSE_PDFIUM_ROOT=$PdfiumRoot")
$configure = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join " "
$targets = "pulse pulse_picker_ui_proof pulse_shell pulse_preview pulse_elevated pulse_dialog_launcher pulse_file_dialog_ui_proof_runtime pulse_dialog_auto pulse_dialog_auto_control pulse_dialog_admin_auto pulse_file_dialog_auto_entry_runtime pulse_dialog_auto_registration_test pulse_dialog_auto_com_activation_test"
$prefixRepair = 'python -X utf8 -B "' + (Join-Path $PSScriptRoot "fix_msvc_dependency_prefix.py") +
    '" --build "' + $buildPath + '" --invalidate-target pulse --invalidate-target pulse_picker_ui_proof' +
    ' --invalidate-target pulse_dialog_auto --invalidate-target pulse_file_dialog_ui_proof_runtime --invalidate-target pulse_localization'
$command = 'chcp 65001 >nul && call "' + $VcVars + '" x64_x86 >nul && set "VSLANG=1033" && cmake ' + $configure +
    ' && ' + $prefixRepair +
    ' && cmake --build "' + $buildPath + '" --target ' + $targets + ' --parallel 4'
Push-Location $repo
try { & cmd.exe /d /c $command; exit $LASTEXITCODE }
finally { Pop-Location }
