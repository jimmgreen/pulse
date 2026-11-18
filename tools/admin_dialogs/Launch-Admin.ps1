param([Parameter(Mandatory=$true)][string]$Target,[switch]$Elevated)
$ErrorActionPreference = 'Stop'
if ($Target.IndexOfAny([char[]]@('"',"`r","`n")) -ge 0 -or -not [IO.Path]::IsPathRooted($Target) -or -not (Test-Path -LiteralPath $Target -PathType Leaf)) { throw 'Drag an existing application EXE onto the launch script.' }
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    if ($Elevated) { throw 'Administrator consent was not granted.' }
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Elevated -Target "' + $Target + '"'
    Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -ArgumentList $arguments -WindowStyle Hidden -Wait
    exit
}
$bundle = Join-Path $PSScriptRoot 'Bundle'
& (Join-Path $bundle 'PulseDialogLauncher.exe') --admin-session --bundle $bundle --cwd (Split-Path -Parent $Target) --control-root (Join-Path $PSScriptRoot 'Sessions') -- $Target
if ($LASTEXITCODE -ne 0) { throw "Administrator launch failed: $LASTEXITCODE" }
