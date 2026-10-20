# Builds the image preview pack (pulse-imgpack.exe + LGPL DLLs).
#   pwsh -File packs\images\build.ps1 [-Architecture x64|arm64] [-VcpkgRoot C:\vcpkg] [-Work build\imgpack]
# Everything vcpkg writes (install tree, build trees, downloads, binary cache)
# stays under -Work: the shared vcpkg checkout and the user profile are never
# written. Output: <Work>\pack\ (flat files, ready for LZMS + catalog).
param(
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'C:\vcpkg' }),
    [string]$Work = '',
    [switch]$SkipVcpkg,
    # Read-only folder of earlier vcpkg downloads (tool and source archives)
    # copied in when missing; saves fetching CMake and friends again.
    [string]$SeedDownloads = '',
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64'
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
$Architecture = $Architecture.ToLowerInvariant()
$nativeArchitecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
if ($nativeArchitecture -ne $Architecture) {
    throw "The pack smoke tests require a native $Architecture Windows runner (OS: $nativeArchitecture)."
}
if (-not $Work) {
    $workName = if ($Architecture -eq 'x64') { 'imgpack' } else { "imgpack-$Architecture" }
    $Work = Join-Path $repo "build\$workName"
}
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path $Work).Path
$triplet = "$Architecture-windows-imgpack"
$hostTriplet = "$Architecture-windows-release"
$installRoot = Join-Path $Work 'installed'
$prefix = Join-Path $installRoot $triplet
$vcpkg = Join-Path $VcpkgRoot 'vcpkg.exe'

function Step($text) { Write-Output "== $text" }
function Assert-PackMachine([string]$Path) {
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($Path))
    try {
        if ($reader.ReadUInt16() -ne 0x5a4d) { throw "Not a PE file: $Path" }
        $reader.BaseStream.Position = 0x3c
        $reader.BaseStream.Position = $reader.ReadInt32()
        if ($reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE signature: $Path" }
        $machine = $reader.ReadUInt16()
        $expected = if ($Architecture -eq 'arm64') { 0xaa64 } else { 0x8664 }
        if ($machine -ne $expected) { throw ('Wrong PE machine in {0}: 0x{1:x4}, expected {2}' -f $Path, $machine, $Architecture) }
    } finally { $reader.Dispose() }
}

Step 'MSVC environment'
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or $env:VSCMD_ARG_TGT_ARCH -ne $Architecture -or $env:VSCMD_ARG_HOST_ARCH -ne $Architecture) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $component = if ($Architecture -eq 'arm64') { 'Microsoft.VisualStudio.Component.VC.Tools.ARM64' } else { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
    $vs = & $vswhere -latest -products * -requires $component -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vs) { throw "Visual Studio $Architecture tools not found" }
    $devcmd = Join-Path ($vs | Select-Object -First 1) 'Common7\Tools\VsDevCmd.bat'
    $environment = cmd /d /s /c "`"`"$devcmd`" -no_logo -arch=$Architecture -host_arch=$Architecture >nul && set`""
    if ($LASTEXITCODE -ne 0) { throw 'MSVC environment setup failed' }
    $environment | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
    }
}
if ($env:VSCMD_ARG_TGT_ARCH -ne $Architecture -or $env:VSCMD_ARG_HOST_ARCH -ne $Architecture) { throw 'MSVC architecture mismatch' }

if (-not $SkipVcpkg) {
    Step "vcpkg install ($triplet; host $hostTriplet)"
    $cache = Join-Path $Work 'binarycache'
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    $env:VCPKG_DEFAULT_BINARY_CACHE = $cache
    $env:VCPKG_DISABLE_METRICS = '1'
    $env:VCPKG_KEEP_ENV_VARS = ''
    $downloads = Join-Path $Work 'downloads'
    New-Item -ItemType Directory -Force -Path $downloads | Out-Null
    if ($SeedDownloads -and (Test-Path $SeedDownloads)) {
        Get-ChildItem $SeedDownloads -File | Where-Object { -not (Test-Path (Join-Path $downloads $_.Name)) } |
            ForEach-Object { Copy-Item $_.FullName $downloads }
    }
    # Downloads through proxies fail now and then; finished ports are kept.
    for ($attempt = 1; $attempt -le 3; ++$attempt) {
        & $vcpkg install --x-manifest-root="$here" --overlay-ports="$here\ports" --overlay-triplets="$here\triplets" `
            --triplet $triplet --host-triplet $hostTriplet `
            --x-install-root="$installRoot" --x-buildtrees-root="$Work\buildtrees" `
            --x-packages-root="$Work\packages" --downloads-root="$downloads" --clean-after-build
        if ($LASTEXITCODE -eq 0) { break }
        if ($attempt -eq 3) { throw "vcpkg install failed ($LASTEXITCODE)" }
        Write-Output "vcpkg install failed ($LASTEXITCODE), retrying"
        Start-Sleep -Seconds 10
    }
}

Step 'configure + build'
$cmakeDir = Join-Path $Work 'cmake'
cmake --fresh -G Ninja -S $here -B $cmakeDir -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$prefix"
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
cmake --build $cmakeDir
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Assert-PackMachine (Join-Path $cmakeDir 'imgpack_test.exe')
& (Join-Path $cmakeDir 'imgpack_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'imgpack_test failed' }

Step 'stage'
$pack = Join-Path $Work 'pack'
if (Test-Path $pack) { Remove-Item -Recurse -Force $pack }
New-Item -ItemType Directory -Force -Path $pack | Out-Null
Copy-Item (Join-Path $cmakeDir 'pulse-imgpack.exe') $pack
Get-ChildItem (Join-Path $prefix 'bin') -Filter *.dll | ForEach-Object { Copy-Item $_.FullName $pack }
Get-ChildItem $pack -File | Where-Object { $_.Extension -in '.exe', '.dll' } | ForEach-Object { Assert-PackMachine $_.FullName }

# Licences of everything linked in (the vcpkg copyright file of each port).
$notices = New-Object System.Text.StringBuilder
[void]$notices.AppendLine('Pulse image preview pack - third-party notices')
[void]$notices.AppendLine('heif.dll and libde265.dll are LGPL-3.0 libraries; you may replace them with')
[void]$notices.AppendLine('compatible builds of the same version. Sources: https://github.com/strukturag')
[void]$notices.AppendLine('')
Get-ChildItem (Join-Path $prefix 'share') -Directory | Sort-Object Name | ForEach-Object {
    $copyright = Join-Path $_.FullName 'copyright'
    if ((Test-Path $copyright) -and ($_.Name -notmatch '^vcpkg-')) {
        [void]$notices.AppendLine(('=' * 78))
        [void]$notices.AppendLine($_.Name)
        [void]$notices.AppendLine(('=' * 78))
        [void]$notices.AppendLine((Get-Content $copyright -Raw))
    }
}
[void]$notices.AppendLine(('=' * 78))
[void]$notices.AppendLine('qoi.h (MIT) - see packs/images/third_party')
[IO.File]::WriteAllText((Join-Path $pack 'THIRD_PARTY_NOTICES.txt'), $notices.ToString(), (New-Object Text.UTF8Encoding $false))

Step 'smoke'
$exe = Join-Path $pack 'pulse-imgpack.exe'
$formats = & $exe formats
if ($LASTEXITCODE -ne 0) { throw 'formats failed' }
$formats | ForEach-Object { Write-Output "  $_" }
if (-not ($formats -match 'hevc=1') -or -not ($formats -match 'av1=1')) { throw 'HEVC or AV1 decoder missing' }
& $exe version
if ($LASTEXITCODE -ne 0) { throw 'version failed' }

Step 'dependencies'
$system = '^(kernel32|user32|advapi32|ole32|shell32|bcrypt|ws2_32|api-ms-win-.*|ucrtbase|ntdll)\.dll$'
foreach ($file in Get-ChildItem $pack -Include *.exe, *.dll -Recurse) {
    $imports = & dumpbin /nologo /dependents $file.FullName
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect imports of $($file.Name)" }
    $deps = $imports | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() }
    foreach ($d in $deps) {
        if ($d -notmatch $system -and -not (Test-Path (Join-Path $pack $d))) { throw "$($file.Name) needs $d" }
    }
    Write-Output ("  {0}: {1}" -f $file.Name, ($deps -join ' '))
}
Get-ChildItem $pack | ForEach-Object {
    Write-Output ("  {0,-26} {1,10:N0}  {2}" -f $_.Name, $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())
}
Write-Output "PACK=$pack"
