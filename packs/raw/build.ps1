param(
    [string]$VcpkgRoot = 'C:\vcpkg',
    [string]$Work = '',
    [switch]$SkipVcpkg,
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
    $workName = if ($Architecture -eq 'x64') { 'rawpack' } else { "rawpack-$Architecture" }
    $Work = Join-Path $repo "build\$workName"
}
New-Item -ItemType Directory -Force $Work | Out-Null
$Work = (Resolve-Path $Work).Path
$triplet = "$Architecture-windows-rawpack"
$hostTriplet = "$Architecture-windows-release"
$prefix = Join-Path $Work "installed\$triplet"

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

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or $env:VSCMD_ARG_TGT_ARCH -ne $Architecture -or $env:VSCMD_ARG_HOST_ARCH -ne $Architecture) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $component = if ($Architecture -eq 'arm64') { 'Microsoft.VisualStudio.Component.VC.Tools.ARM64' } else { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
    $vs = & $vswhere -latest -products * -requires $component -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vs) { throw "Visual Studio $Architecture tools not found" }
    $devcmd = Join-Path ($vs | Select-Object -First 1) 'Common7\Tools\VsDevCmd.bat'
    $environment = cmd /d /c "call `"$devcmd`" -no_logo -arch=$Architecture -host_arch=$Architecture >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'MSVC environment setup failed' }
    $environment | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
    }
}
if ($env:VSCMD_ARG_TGT_ARCH -ne $Architecture -or $env:VSCMD_ARG_HOST_ARCH -ne $Architecture) { throw 'MSVC architecture mismatch' }

$env:VCPKG_DISABLE_METRICS = '1'
$env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $Work 'binarycache'
New-Item -ItemType Directory -Force $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
if (-not $SkipVcpkg) {
    & "$VcpkgRoot\vcpkg.exe" install --x-manifest-root="$here" --overlay-triplets="$here\triplets" --triplet $triplet --host-triplet $hostTriplet --x-install-root="$Work\installed" --x-buildtrees-root="$Work\buildtrees" --x-packages-root="$Work\packages" --downloads-root="$Work\downloads" --clean-after-build
    if ($LASTEXITCODE) { throw 'vcpkg install failed' }
}
cmake --fresh -G Ninja -S $here -B "$Work\cmake" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$prefix" "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake" "-DVCPKG_INSTALLED_DIR=$Work\installed" "-DVCPKG_TARGET_TRIPLET=$triplet" "-DVCPKG_HOST_TRIPLET=$hostTriplet" "-DVCPKG_OVERLAY_TRIPLETS=$here\triplets" -DVCPKG_MANIFEST_INSTALL=OFF
if ($LASTEXITCODE) { throw 'configure failed' }
cmake --build "$Work\cmake" --target pulse-rawpack
if ($LASTEXITCODE) { throw 'build failed' }
# Stage to a new/reusable directory without deleting any user supplied path.
$pack = Join-Path $Work 'pack'
New-Item -ItemType Directory -Force $pack | Out-Null
Copy-Item "$Work\cmake\pulse-rawpack.exe" $pack -Force
Copy-Item "$prefix\bin\raw_r.dll" $pack -Force
# Both helper and replaceable LibRaw DLL use static CRT; verify staged binaries.
Get-ChildItem $pack -File | Where-Object { $_.Extension -in '.exe', '.dll' } | ForEach-Object {
    Assert-PackMachine $_.FullName
    $imports = & dumpbin /nologo /dependents $_.FullName
    if ($LASTEXITCODE) { throw "Cannot inspect imports of $($_.Name)" }
    $dlls = $imports | Where-Object { $_ -match '^\s+[^\s]+\.dll\s*$' }
    Write-Output "Imports for $($_.Name): $($dlls.Trim() -join ', ')"
    if ($dlls -match '(?i)msvcp[0-9]|vcruntime|ucrtbase|api-ms-win-crt-') {
        throw "Unexpected dynamic VC runtime dependency in $($_.Name)"
    }
}
$notices = [System.Text.StringBuilder]::new()
[void]$notices.AppendLine('Pulse RAW preview pack; LibRaw is a replaceable LGPL/CDDL library. https://www.libraw.org/')
Get-ChildItem "$prefix\share" -Directory | Sort-Object Name | ForEach-Object {
    $license = Join-Path $_.FullName 'copyright'
    if (Test-Path $license) { [void]$notices.AppendLine($_.Name); [void]$notices.AppendLine((Get-Content $license -Raw)) }
}
[IO.File]::WriteAllText((Join-Path $pack 'THIRD-PARTY-NOTICES.txt'), $notices.ToString())
& "$here\test.ps1" -Exe "$pack\pulse-rawpack.exe" -Work "$Work\test"
if ($LASTEXITCODE) { throw 'rawpack test failed' }
Write-Output "RAW pack staged at $pack"
