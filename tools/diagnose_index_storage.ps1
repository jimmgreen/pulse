# Pulse index diagnosis. Read-only except for a NEW report file in OutputDirectory.
# No service restart, ACL change, USN-journal creation, index deletion or upload.
[CmdletBinding()]
param(
    [string]$IndexDirectory,
    [string]$InstallDirectory,
    [string]$ConfigurationFile,
    [string]$OutputDirectory = $PSScriptRoot,
    [string[]]$DriveLetters,
    [switch]$ProbeVolumes,
    [switch]$SkipSystemProbes,
    [switch]$SkipLogs
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
# Windows PowerShell may bind parameter defaults before PSScriptRoot is populated.
if (-not $PSBoundParameters.ContainsKey('OutputDirectory')) { $OutputDirectory = $PSScriptRoot }

function ErrorCode($record) {
    if ($record.Exception -is [System.ComponentModel.Win32Exception]) { return $record.Exception.NativeErrorCode }
    return ($record.Exception.HResult -band 0xffff)
}
function PathShape([string]$path) {
    if ([string]::IsNullOrWhiteSpace($path)) { return $null }
    try {
        $full = [IO.Path]::GetFullPath($path)
        return [ordered]@{
            is_absolute = [IO.Path]::IsPathRooted($path)
            drive = if ($full -match '^([a-zA-Z]:)') { $Matches[1].ToUpperInvariant() } else { '<UNC-or-other>' }
            length = $full.Length
            has_non_ascii = [bool]($full -match '[^\x00-\x7f]')
            has_trailing_separator = [bool]($path -match '[\\/]$')
            is_drive_root = ($full.TrimEnd('\') -eq [IO.Path]::GetPathRoot($full).TrimEnd('\'))
        }
    } catch { return [ordered]@{ invalid = $true; error = (ErrorCode $_) } }
}
function DirectoryInfo([string]$path) {
    $row = [ordered]@{ shape = (PathShape $path); exists = $false }
    if ([string]::IsNullOrWhiteSpace($path)) { return $row }
    try {
        # Do not authenticate to a network share or follow a junction while collecting metadata.
        $local = $path -replace '^\\\\\?\\', ''
        if ($local -notmatch '^[a-zA-Z]:[\\/]') { $row.blocked = 'not-a-local-absolute-drive-path'; return $row }
        $full = [IO.Path]::GetFullPath($local)
        $drive = New-Object IO.DriveInfo([IO.Path]::GetPathRoot($full))
        if ($drive.DriveType -eq [IO.DriveType]::Network) { $row.blocked = 'network-drive'; return $row }
        $part = [IO.Path]::GetPathRoot($full)
        foreach ($component in $full.Substring($part.Length).Split('\')) {
            if (-not $component) { continue }
            $part = Join-Path $part $component
            $ancestor = Get-Item -LiteralPath $part -Force
            if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { $row.ancestors_have_reparse_point = $true; $row.blocked = 'reparse-path'; return $row }
        }
        $item = Get-Item -LiteralPath $full -Force
        $row.exists = $true
        $row.is_directory = $item.PSIsContainer
        $row.is_reparse = [bool]($item.Attributes -band [IO.FileAttributes]::ReparsePoint)
        if (-not $item.PSIsContainer) { return $row }
        $acl = Get-Acl -LiteralPath $path
        $row.dacl_protected = $acl.AreAccessRulesProtected
        $row.owner_kind = 'other'
        try {
            $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
            if ($owner -eq 'S-1-5-18') { $row.owner_kind = 'system' }
            if ($owner -eq 'S-1-5-32-544') { $row.owner_kind = 'administrators' }
        } catch {}
        $row.system_full_control = $false
        $row.administrators_full_control = $false
        foreach ($rule in $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
            $allowed = $rule.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow
            $full = ($rule.FileSystemRights -band [Security.AccessControl.FileSystemRights]::FullControl) -eq [Security.AccessControl.FileSystemRights]::FullControl
            if ($allowed -and $full -and $rule.IdentityReference.Value -eq 'S-1-5-18') { $row.system_full_control = $true }
            if ($allowed -and $full -and $rule.IdentityReference.Value -eq 'S-1-5-32-544') { $row.administrators_full_control = $true }
        }
        $row.ancestors_have_reparse_point = $false
        for ($part = $item; $null -ne $part; $part = $part.Parent) {
            if ($part.Attributes -band [IO.FileAttributes]::ReparsePoint) { $row.ancestors_have_reparse_point = $true }
        }
    } catch { $row.inspection_error = (ErrorCode $_) }
    return $row
}
function SafeRuntimeEvents([string]$root) {
    $result = (New-Object 'System.Collections.Generic.List[object]')
    if ([string]::IsNullOrWhiteSpace($root)) { return @() }
    try {
        $files = @(Get-ChildItem -LiteralPath $root -Filter 'index-*.jsonl*' -File -ErrorAction Stop | Sort-Object LastWriteTime -Descending | Select-Object -First 3)
        foreach ($file in $files) {
            # Keep failure/rebuild evidence independently of periodic health and totals.
            # Stream each file so both working memory and the exported sample are bounded.
            $critical = New-Object 'System.Collections.Generic.Queue[object]'
            $recent = New-Object 'System.Collections.Generic.Queue[object]'
            $sequence = 0L
            try { Get-Content -LiteralPath $file.FullName -Encoding UTF8 | ForEach-Object {
                try {
                    $sequence++
                    $record = $_ | ConvertFrom-Json
                    if ($record.event -notin 'index_rebuild_begin','index_rebuild_end','index_volume_scan_failed','index_stage_totals','process_start','process_stop','process_health') { return }
                    $fields = [ordered]@{}
                    if ($record.data) {
                        foreach ($property in $record.data.PSObject.Properties) {
                            if ($property.Name -match '^[a-zA-Z0-9_]{1,48}$' -and $property.Value -is [ValueType] -and $property.Value -isnot [char]) { $fields[$property.Name] = $property.Value }
                        }
                    }
                    $safe = [ordered]@{ event = [string]$record.event; data = $fields }
                    if ([string]$record.version -match '^\d+(\.\d+){1,3}$') { $safe.version = [string]$record.version }
                    $important = $record.event -in 'index_rebuild_begin','index_rebuild_end','index_volume_scan_failed'
                    $queue = $critical
                    if (-not $important) { $queue = $recent }
                    $capacity = if ($important) { 64 } else { 16 }
                    $queue.Enqueue(@{ sequence = $sequence; value = $safe })
                    if ($queue.Count -gt $capacity) { $null = $queue.Dequeue() }
                } catch {}
            } } catch {}
            foreach ($entry in (@($critical.ToArray()) + @($recent.ToArray()) | Sort-Object { $_.sequence })) { $result.Add($entry.value) }
        }
    } catch {}
    return $result.ToArray()
}

$report = [ordered]@{
    schema = 1
    generated_utc = [DateTime]::UtcNow.ToString('o')
    diagnostic_version = '1.2'
    privacy = 'No document contents, filenames, usernames, full index paths or upload; only a new report is written.'
    powershell_version = $PSVersionTable.PSVersion.ToString()
    configuration = [ordered]@{ found = $false }
    service = [ordered]@{ checked = $false }
    binaries = @()
    volumes = @()
    runtime_events = @()
    notes = @()
}
$notes = (New-Object 'System.Collections.Generic.List[string]')
$config = $null
if (-not $ConfigurationFile -and -not $SkipSystemProbes) {
    $ConfigurationFile = Join-Path $env:ProgramData 'Pulse\index-config.json'
    if (-not (Test-Path -LiteralPath $ConfigurationFile)) { $ConfigurationFile = Join-Path $env:ProgramData 'Pulse\Index\config.json' }
}
if ($ConfigurationFile) {
    try {
        $config = Get-Content -LiteralPath $ConfigurationFile -Encoding UTF8 -Raw | ConvertFrom-Json
        $report.configuration.found = $true
        $report.configuration.generation = $config.generation
        $report.configuration.version = $config.version
        $report.configuration.excluded_volume_count = if ($null -eq $config.excluded_volume_ids) { 0 } else { @($config.excluded_volume_ids).Count }
        if (-not $IndexDirectory -and $config.index_path -is [string]) { $IndexDirectory = $config.index_path }
    } catch { $report.configuration.error = (ErrorCode $_) }
}
$report.index_directory = DirectoryInfo $IndexDirectory
$elevated = $false
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $elevated = (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    $identity.Dispose()
} catch {}
$report.elevated = $elevated
if (-not $SkipSystemProbes) {
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        $report.os = [ordered]@{ version = $os.Version; build = $os.BuildNumber; architecture = $os.OSArchitecture; total_memory_kib = $os.TotalVisibleMemorySize; free_memory_kib = $os.FreePhysicalMemory }
    } catch { $report.os_error = (ErrorCode $_) }
    try {
        $service = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
        $report.service.checked = $true
        $report.service.installed = $null -ne $service
        if ($service) {
            $report.service.state = $service.State
            $report.service.start_mode = $service.StartMode
            $report.service.win32_exit_code = $service.ExitCode
            $report.service.specific_exit_code = $service.ServiceSpecificExitCode
            $report.service.account_kind = if ($service.StartName -eq 'LocalSystem') { 'LocalSystem' } else { 'other' }
            if (-not $InstallDirectory) {
                $match = [regex]::Match($service.PathName, '^"([^"]+\.exe)"|^(.+?\.exe)(?:\s|$)', [Text.RegularExpressions.RegexOptions]::IgnoreCase)
                if ($match.Success) {
                    $executable = if ($match.Groups[1].Success) { $match.Groups[1].Value } else { $match.Groups[2].Value }
                    $InstallDirectory = [IO.Path]::GetDirectoryName($executable)
                }
            }
        }
    } catch { $report.service.error = (ErrorCode $_) }
    try {
        $disks = @(Get-CimInstance Win32_LogicalDisk | Where-Object { $_.DriveType -in 2,3 })
        $report.volumes = @($disks | ForEach-Object { [ordered]@{ drive = $_.DeviceID; file_system = $_.FileSystem; drive_type = $_.DriveType; size_bytes = $_.Size; free_bytes = $_.FreeSpace } })
        if (-not $DriveLetters) { $DriveLetters = @($disks | Where-Object { $_.FileSystem -eq 'NTFS' } | ForEach-Object { $_.DeviceID.TrimEnd(':') }) }
    } catch { $report.volumes_error = (ErrorCode $_) }
}
if ($InstallDirectory) {
    $report.binaries = @(foreach ($name in 'Pulse.exe','Pulse.Index.exe') {
        $file = Join-Path $InstallDirectory $name
        $row = [ordered]@{ name = $name; exists = $false }
        try { $info = Get-Item -LiteralPath $file; $row.exists = $true; $row.size_bytes = $info.Length; $row.file_version = $info.VersionInfo.FileVersion; $row.product_version = $info.VersionInfo.ProductVersion } catch { $row.error = (ErrorCode $_) }
        $row
    })
}
if (-not $SkipLogs -and -not $SkipSystemProbes) {
    $report.runtime_events = @(SafeRuntimeEvents (Join-Path $env:ProgramData 'Pulse\Diagnostics\Runtime'))
    if ($report.runtime_events.Count -eq 0) { $notes.Add('No readable structured index runtime events. This does not establish the failure cause.') }
}
if ($ProbeVolumes -and -not $SkipSystemProbes -and $elevated) {
    # Matches production OpenVolume and probes ONLY filesystem metadata.
    # The returned USN page is discarded; no filenames or records are exported.
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class PulseIndexReadOnlyProbe {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] input, uint inputLength, byte[] output, uint outputLength, out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool SetFilePointerEx(SafeFileHandle h, long distance, out long position, uint method);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool ReadFile(SafeFileHandle h, byte[] buffer, uint size, out uint returned, IntPtr overlapped);
    public static object Probe(string letter) {
        var r = new Dictionary<string,object>(); r["drive"] = letter + ":";
        using (var h = CreateFile(@"\\.\" + letter + ":", 1, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (h.IsInvalid) { r["open_error"] = Marshal.GetLastWin32Error(); return r; }
            r["open_error"] = 0;
            var geometry = new byte[96]; uint returned;
            bool ok = DeviceIoControl(h, 0x90064, null, 0, geometry, (uint)geometry.Length, out returned, IntPtr.Zero);
            r["ntfs_geometry_error"] = ok ? 0 : Marshal.GetLastWin32Error();
            if (ok && returned >= 96) {
                uint sector = BitConverter.ToUInt32(geometry,40), cluster = BitConverter.ToUInt32(geometry,44), record = BitConverter.ToUInt32(geometry,48);
                long length = BitConverter.ToInt64(geometry,56), lcn = BitConverter.ToInt64(geometry,64);
                r["bytes_per_sector"] = sector; r["bytes_per_cluster"] = cluster; r["bytes_per_record"] = record; r["mft_valid_bytes"] = length;
                if (lcn >= 0 && cluster > 0 && record >= 512 && record <= 4096 && lcn <= long.MaxValue / cluster) {
                    long position; var buffer = new byte[record];
                    bool seek = SetFilePointerEx(h,lcn * cluster,out position,0);
                    int error = seek ? 0 : Marshal.GetLastWin32Error();
                    if (seek) { bool read = ReadFile(h,buffer,record,out returned,IntPtr.Zero); error = read && returned == record ? 0 : read ? 30 : Marshal.GetLastWin32Error(); }
                    r["first_mft_record_read_error"] = error;
                    if (error == 0) r["first_mft_record_has_file_signature"] = BitConverter.ToUInt32(buffer,0) == 0x454c4946;
                    Array.Clear(buffer,0,buffer.Length);
                }
            }
            var journal = new byte[128];
            ok = DeviceIoControl(h,0x900f4,null,0,journal,(uint)journal.Length,out returned,IntPtr.Zero);
            r["query_usn_error"] = ok ? 0 : Marshal.GetLastWin32Error();
            var input = new byte[24]; Array.Copy(BitConverter.GetBytes(long.MaxValue),0,input,16,8);
            var page = new byte[65536];
            ok = DeviceIoControl(h,0x900b3,input,(uint)input.Length,page,(uint)page.Length,out returned,IntPtr.Zero);
            r["first_usn_page_error"] = ok ? 0 : Marshal.GetLastWin32Error();
            if (ok) { r["first_usn_page_bytes"] = returned; if (returned >= 14) r["first_usn_record_major_version"] = BitConverter.ToUInt16(page,12); }
            Array.Clear(page,0,page.Length); Array.Clear(journal,0,journal.Length);
        }
        return r;
    }
}
'@
    $report.raw_metadata_probes = @(foreach ($letter in ($DriveLetters | Select-Object -Unique | Select-Object -First 16)) {
        $clean = $letter.Trim().TrimEnd(':','\').ToUpperInvariant()
        if ($clean -notmatch '^[A-Z]$') { throw 'DriveLetters must contain only drive letters.' }
        [PulseIndexReadOnlyProbe]::Probe($clean)
    })
} elseif ($ProbeVolumes) {
    $notes.Add('Raw-volume metadata probe skipped: requires an already-elevated PowerShell and system probes enabled. The script never elevates itself.')
}
if (-not $elevated) { $notes.Add('A normal user may be unable to inspect a private index folder. This alone does not prove that the SYSTEM index service lacks access.') }
$report.notes = $notes.ToArray()
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { throw 'OutputDirectory must be specified.' }
try { $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory) }
catch { throw 'OutputDirectory is invalid. Do not pass a quoted directory ending in a backslash; omit OutputDirectory to use the script folder.' }
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$name = 'PulseIndexReport-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8) + '.json'
$output = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) $name
$json = $report | ConvertTo-Json -Depth 12
$stream = [IO.File]::Open($output,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
try { $encoding = New-Object Text.UTF8Encoding($false); $writer = New-Object IO.StreamWriter($stream,$encoding); $writer.Write($json); $writer.Flush() } finally { if ($writer) { $writer.Dispose() } else { $stream.Dispose() } }
Write-Output ('REPORT_FILE=' + $output)
Write-Output 'No indexes, preferences, permissions, services or journals were changed. Review the JSON before sharing it.'
