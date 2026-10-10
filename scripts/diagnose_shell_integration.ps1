# Run on the affected computer, as the affected user. No elevation is needed.
# Reads only the associations used by Pulse; writes one diagnostic JSON file.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Read-SelectedRegistryValues {
    param($Base, [string]$Path, [string[]]$Names)
    $result = [ordered]@{ path = $Path; exists = $false; values = @() }
    $key = $null
    try {
        $key = $Base.OpenSubKey($Path, $false)
        if ($null -eq $key) { return $result }
        $result.exists = $true
        $present = $key.GetValueNames()
        foreach ($name in $Names) {
            $value = [ordered]@{ name = $name; exists = ($present -contains $name) }
            if ($value.exists) {
                $value.kind = $key.GetValueKind($name).ToString()
                $raw = $key.GetValue($name, $null,
                    [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                if ($raw -is [byte[]]) {
                    $value.encoding = 'base64'
                    $value.data = [Convert]::ToBase64String($raw)
                } else {
                    $value.data = $raw
                }
            }
            $result.values += $value
        }
    } catch {
        $result.error = $_.Exception.Message
    } finally {
        if ($null -ne $key) { $key.Dispose() }
    }
    return $result
}

$groups = @(
    @{ name = 'Folder'; key = 'Folder'; verb = 'open' },
    @{ name = 'Directory'; key = 'Directory'; verb = 'open' },
    @{ name = 'Drive'; key = 'Drive'; verb = 'open' },
    @{ name = 'WinE'; key = 'CLSID\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}'; verb = 'opennewwindow' },
    @{ name = 'ThisPc'; key = 'CLSID\{20D04FE0-3AEA-1069-A2D8-08002B30309D}'; verb = 'open' },
    @{ name = 'RecycleBin'; key = 'CLSID\{645FF040-5081-101B-9F08-00AA002F954E}'; verb = 'open' }
)
$views = @([Microsoft.Win32.RegistryView]::Registry32)
if ([Environment]::Is64BitOperatingSystem) {
    $views = @([Microsoft.Win32.RegistryView]::Registry64,
               [Microsoft.Win32.RegistryView]::Registry32)
}
$report = [ordered]@{
    schema = 1
    captured_utc = [DateTime]::UtcNow.ToString('o')
    os_version = [Environment]::OSVersion.Version.ToString()
    process_64_bit = [Environment]::Is64BitProcess
    note = 'Registry reads only. Paths inside association commands and Pulse backups may identify installation locations.'
    associations = @()
    backups = @()
}
foreach ($view in $views) {
    foreach ($hive in @('CurrentUser', 'LocalMachine', 'ClassesRoot')) {
        $base = $null
        try {
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
                [Microsoft.Win32.RegistryHive]::$hive, $view)
            $prefix = 'Software\Classes\'
            if ($hive -eq 'ClassesRoot') { $prefix = '' }
            foreach ($group in $groups) {
                $shell = $prefix + $group.key + '\shell'
                $verb = $shell + '\' + $group.verb
                $entry = [ordered]@{ hive = $hive; view = $view.ToString(); group = $group.name; keys = @() }
                foreach ($path in @($shell, $verb, ($verb + '\command'))) {
                    $entry.keys += Read-SelectedRegistryValues $base $path @('', 'DelegateExecute', 'PulseBackup', 'PulseBackupDelegateExecute')
                }
                $report.associations += $entry
                if ($hive -eq 'CurrentUser' -and $group.name -ne 'Folder') {
                    $backup = Read-SelectedRegistryValues $base (
                        'Software\Pulse\ShellIntegration\Backups\v1\' + $group.name) @('Snapshot', 'PendingUpgrade')
                    $report.backups += [ordered]@{ view = $view.ToString(); group = $group.name; key = $backup }
                }
            }
        } catch {
            $report.associations += [ordered]@{ hive = $hive; view = $view.ToString(); error = $_.Exception.Message }
        } finally {
            if ($null -ne $base) { $base.Dispose() }
        }
    }
}

$destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
if (Test-Path -LiteralPath $destination) {
    throw "Output already exists. Choose a new filename: $destination"
}
$json = $report | ConvertTo-Json -Depth 12
[IO.File]::WriteAllText($destination, $json, [Text.UTF8Encoding]::new($false))
Write-Output "Diagnostic report saved: $destination"
