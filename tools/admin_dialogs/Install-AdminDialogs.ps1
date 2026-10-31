param([switch]$Elevated,[switch]$RunSelfTest,[ValidateRange(0,100)][int]$Revision=0)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    if ($Elevated) { throw 'Administrator consent was not granted.' }
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Elevated'
    if ($RunSelfTest) { $arguments += ' -RunSelfTest' }
    if ($Revision) { $arguments += ' -Revision ' + $Revision }
    Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -ArgumentList $arguments -WindowStyle Hidden -Wait
    exit
}
$source = Join-Path $PSScriptRoot 'Bundle'
function Assert-NoLinks([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        $item = Get-Item -LiteralPath $current -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Package path links are not supported.' }
        $current = Split-Path -Parent $current
    }
}
Assert-NoLinks $source
$metadataHashes = @{}
foreach ($name in @('manifest.json','VERIFIED.json')) {
    Assert-NoLinks (Join-Path $source $name)
    $metadataHashes[$name] = (Get-FileHash -LiteralPath (Join-Path $source $name) -Algorithm SHA256).Hash
}
$manifest = Get-Content -LiteralPath (Join-Path $source 'manifest.json') -Raw | ConvertFrom-Json
if ((Get-FileHash -LiteralPath (Join-Path $source 'manifest.json') -Algorithm SHA256).Hash -ne $metadataHashes['manifest.json']) { throw 'Manifest changed during verification.' }
if ($manifest.architecture -ne 'x64') { throw 'This administrator installer requires the x64 bundle.' }
# Verify the explicitly selected package before copying; the destination ACL is
# the continuing trust boundary, not this user-writable manifest.
foreach ($entry in $manifest.files) {
    $relative = [string]$entry.path
    if ([IO.Path]::IsPathRooted($relative) -or $relative.Contains('..') -or $relative.Contains(':')) { throw 'Invalid package path.' }
    $file = Get-Item -LiteralPath (Join-Path $source $relative)
    Assert-NoLinks $file.FullName
    if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Package links are not supported.' }
    if ((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Package checksum mismatch: $relative" }
}
$programFiles = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)
$install = Join-Path $programFiles 'PulseDialogBridge-20261031-r5'
if ($Revision) {
    Assert-NoLinks $install
    & (Join-Path $install 'Bundle\PulseDialogLauncher.exe') --admin-check $install
    if ($LASTEXITCODE -ne 0) { throw 'Existing protected installation is not valid.' }
    $install = Join-Path $install ('Revision' + $Revision)
}
Assert-NoLinks $programFiles
if (Test-Path -LiteralPath $install) { throw 'Installation destination already exists.' }
$adminSid = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
function Protect-Directory([string]$Path) {
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetOwner($adminSid)
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($rule in @(@('S-1-5-18','FullControl'),@('S-1-5-32-544','FullControl'),@('S-1-5-32-545','ReadAndExecute'))) {
        $sid = [Security.Principal.SecurityIdentifier]::new($rule[0])
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,$rule[1],'ContainerInherit,ObjectInherit','None','Allow'))
    }
    # Atomic creation with the protected descriptor; no user-writable interval.
    [IO.Directory]::CreateDirectory($Path,$acl) | Out-Null
}
Protect-Directory $install
Start-Transcript -LiteralPath (Join-Path $install 'install.log') | Out-Null
Protect-Directory (Join-Path $install 'Bundle')
Protect-Directory (Join-Path $install 'Sessions')
foreach ($relative in @($manifest.files.path) + @('manifest.json','VERIFIED.json')) {
    $destination = Join-Path (Join-Path $install 'Bundle') $relative
    $parent = Split-Path -Parent $destination
    if (-not (Test-Path -LiteralPath $parent)) { Protect-Directory $parent }
    Copy-Item -LiteralPath (Join-Path $source $relative) -Destination $destination
    $acl = Get-Acl -LiteralPath $destination
    $acl.SetOwner($adminSid)
    Set-Acl -LiteralPath $destination -AclObject $acl
}
foreach ($entry in $manifest.files) {
    if ((Get-FileHash -LiteralPath (Join-Path (Join-Path $install 'Bundle') $entry.path) -Algorithm SHA256).Hash -ne $entry.sha256) { throw 'Installed checksum mismatch.' }
}
foreach ($name in @('manifest.json','VERIFIED.json')) {
    if ((Get-FileHash -LiteralPath (Join-Path (Join-Path $install 'Bundle') $name) -Algorithm SHA256).Hash -ne $metadataHashes[$name]) { throw 'Installed metadata changed during copy.' }
}
Assert-NoLinks (Join-Path $PSScriptRoot 'Launch-Admin.ps1')
$launchHash = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'Launch-Admin.ps1') -Algorithm SHA256).Hash
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Launch-Admin.ps1') -Destination $install
$script = Join-Path $install 'Launch-Admin.ps1'
if ((Get-FileHash -LiteralPath $script -Algorithm SHA256).Hash -ne $launchHash) { throw 'Launch script changed during copy.' }
$acl = Get-Acl -LiteralPath $script; $acl.SetOwner($adminSid); Set-Acl -LiteralPath $script -AclObject $acl
$launcher = Join-Path $install 'Bundle\PulseDialogLauncher.exe'
& $launcher --admin-check $install
if ($LASTEXITCODE -ne 0) { throw 'Protected installation verification failed. Nothing was enabled.' }
$cmd = '@echo off' + "`r`n" + 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + $script + '" -Target "%~1"' + "`r`n" + 'pause' + "`r`n"
[IO.File]::WriteAllText((Join-Path $install 'Launch-Admin.cmd'),$cmd,[Text.Encoding]::Default)
if ($RunSelfTest) {
    $probe = Join-Path $PSScriptRoot 'pulse_dialog_managed_entry_smoke.exe'
    Assert-NoLinks $probe
    $probeHash = (Get-FileHash -LiteralPath $probe -Algorithm SHA256).Hash
    $test = Join-Path $install 'pulse_dialog_managed_entry_smoke.exe'
    Copy-Item -LiteralPath $probe -Destination $test
    $acl = Get-Acl -LiteralPath $test; $acl.SetOwner($adminSid); Set-Acl -LiteralPath $test -AclObject $acl
    if ((Get-FileHash -LiteralPath $test -Algorithm SHA256).Hash -ne $probeHash) { throw 'Test copy checksum mismatch.' }
    $fixtures = Join-Path $install 'bench_data'; Protect-Directory $fixtures
    $report = Join-Path $fixtures 'admin-save-report.log'
    & $launcher --admin-session --bundle (Join-Path $install 'Bundle') --cwd $fixtures --control-root (Join-Path $install 'Sessions') -- $test --report $report --folder $fixtures --save
    if ($LASTEXITCODE -ne 0) { throw 'Administrator test launch failed.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(50)
    do {
        Start-Sleep -Milliseconds 500
        [string]$text = if (Test-Path -LiteralPath $report) { Get-Content -LiteralPath $report -Raw } else { '' }
    } until ($text.Contains('PASS legacy Pulse window and cancellation') -or $text.Contains('FAIL') -or [DateTime]::UtcNow -gt $deadline)
    if ($text.Contains('FAIL') -or -not $text.Contains('PASS legacy Pulse window and cancellation') -or
        -not $text.Contains('PASS COM Pulse window and cancellation') -or -not $text.Contains('INFO elevation_known=1 elevated=1 save=1')) { throw 'Administrator Save test did not pass; see protected report.' }
    Write-Host 'Administrator Save UI and cancellation passed.'
    $minimalReport = Join-Path $fixtures 'admin-minimal-save-report.log'
    & $launcher --admin-session --bundle (Join-Path $install 'Bundle') --cwd $fixtures --control-root (Join-Path $install 'Sessions') -- $test --report $minimalReport --folder $fixtures --minimal-save
    if ($LASTEXITCODE -ne 0) { throw 'Minimal Save test launch failed.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 500
        [string]$text = if (Test-Path -LiteralPath $minimalReport) { Get-Content -LiteralPath $minimalReport -Raw } else { '' }
    } until ($text.Contains('PASS COM Pulse window and cancellation') -or $text.Contains('FAIL') -or [DateTime]::UtcNow -gt $deadline)
    if ($text.Contains('FAIL') -or -not $text.Contains('PASS COM Pulse window and cancellation') -or -not $text.Contains('INFO minimal_save=1')) { throw 'Minimal Save test did not pass.' }
    Write-Host 'Exact minimal Save contract UI and cancellation passed.'
}
Write-Host "Installed: $install"
Write-Host 'Drag the target EXE onto the installed Launch-Admin.cmd. This does not enable global administrator interception.'
Stop-Transcript | Out-Null
