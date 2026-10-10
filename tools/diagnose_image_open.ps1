#requires -Version 5.1
<#
Read-only image-open diagnostics for Pulse. Does not launch Paint/Pulse, modify
associations/registry, install software, request elevation or upload anything.
Only creates the requested report directory. Image contents are never read.
#>
[CmdletBinding()]
param(
    [string]$ImagePath = '',
    [ValidatePattern('^\.[A-Za-z0-9]{1,15}$')][string]$Extension = '.png',
    [string]$OutputDirectory = '',
    [string]$DataRoot = '',
    [ValidateRange(1, 240)][int]$Minutes = 30,
    [switch]$NonInteractive,
    [switch]$SkipEventLogs
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$warnings = New-Object 'System.Collections.Generic.List[string]'
$startUtc = [DateTime]::UtcNow.AddMinutes(-$Minutes)

function Protect-Text([string]$Text) {
    if ($null -eq $Text) { return $null }
    if ($ImagePath) { $Text = [regex]::Replace($Text, [regex]::Escape($ImagePath), '<IMAGE>', 'IgnoreCase') }
    if ($env:USERPROFILE) { $Text = [regex]::Replace($Text, [regex]::Escape($env:USERPROFILE), '<USERPROFILE>', 'IgnoreCase') }
    $Text = [regex]::Replace($Text, 'S-1-5(?:-\d+)+', '<SID>')
    $Text = [regex]::Replace($Text, '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}', '<EMAIL>')
    return $Text
}
function Warn([string]$Message) { $warnings.Add((Protect-Text $Message)) }
function Value($Key, [string]$Name = '') {
    if ($null -eq $Key) { return $null }
    $v = $Key.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    if ($v -is [string]) { return (Protect-Text $v) }
    return $v
}
function Read-Registry([string]$Hive, [string]$Path, [string]$Name = '', [string]$View = 'Default') {
    $base = $null; $key = $null
    try {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::$Hive, [Microsoft.Win32.RegistryView]::$View)
        $key = $base.OpenSubKey($Path, $false)
        return (Value $key $Name)
    } catch { Warn "Registry read $Hive/$View/$Path : $($_.Exception.Message)" }
    finally { if ($key) { $key.Dispose() }; if ($base) { $base.Dispose() } }
}
function Has-RegistryValue([string]$Hive, [string]$Path, [string]$Name, [string]$View) {
    $base = $null; $key = $null
    try {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::$Hive, [Microsoft.Win32.RegistryView]::$View)
        $key = $base.OpenSubKey($Path, $false)
        if (-not $key) { return $false }
        return ($key.GetValueNames() -contains $Name)
    } catch { Warn "Registry value names $Hive/$View/$Path : $($_.Exception.Message)"; return $null }
    finally { if ($key) { $key.Dispose() }; if ($base) { $base.Dispose() } }
}
function Class-Snapshot([string]$ProgId, [string]$View) {
    $base = $null; $key = $null; $shell = $null
    $r = [ordered]@{ ProgId = $ProgId; View = $View; Exists = $false; DefaultVerb = $null; Verbs = @() }
    try {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::ClassesRoot, [Microsoft.Win32.RegistryView]::$View)
        $key = $base.OpenSubKey($ProgId, $false)
        if (-not $key) { return [pscustomobject]$r }
        $r.Exists = $true; $r.DisplayName = Value $key
        $r.AppUserModelID = Value $key 'AppUserModelID'
        $shell = $key.OpenSubKey('shell', $false)
        if ($shell) {
            $r.DefaultVerb = Value $shell
            foreach ($verb in @($shell.GetSubKeyNames() | Select-Object -First 20)) {
                $vk = $null; $ck = $null; $dk = $null; $ak = $null; $tk = $null
                try {
                    $vk = $shell.OpenSubKey($verb, $false)
                    $ck = $vk.OpenSubKey('command', $false)
                    $dk = $vk.OpenSubKey('ddeexec', $false)
                    if ($dk) { $ak = $dk.OpenSubKey('Application', $false); $tk = $dk.OpenSubKey('Topic', $false) }
                    $r.Verbs += [pscustomobject][ordered]@{
                        Name = $verb; DisplayName = Value $vk 'MUIVerb'
                        Command = Value $ck; DelegateExecute = Value $ck 'DelegateExecute'
                        ExplorerCommandHandler = Value $vk 'ExplorerCommandHandler'
                        DdeCommand = Value $dk; DdeApplication = Value $ak; DdeTopic = Value $tk
                    }
                } finally { foreach ($k in @($tk, $ak, $dk, $ck, $vk)) { if ($k) { $k.Dispose() } } }
            }
        }
    } catch { $r.Error = Protect-Text $_.Exception.Message; Warn "Class read $ProgId/$View : $($_.Exception.Message)" }
    finally { if ($shell) { $shell.Dispose() }; if ($key) { $key.Dispose() }; if ($base) { $base.Dispose() } }
    return [pscustomobject]$r
}
function Choose([string]$Question, [string[]]$Options) {
    Write-Host "`n$Question"
    for ($i = 0; $i -lt $Options.Length; $i++) { Write-Host ('  {0}. {1}' -f ($i + 1), $Options[$i]) }
    $answer = Read-Host '输入编号；不清楚可直接回车'
    $number = 0
    if ([int]::TryParse($answer, [ref]$number) -and $number -ge 1 -and $number -le $Options.Length) {
        return $Options[$number - 1]
    }
    return '未确认'
}

Write-Host 'Pulse 图片打开诊断（不修改注册表或默认应用，不启动图片）' -ForegroundColor Cyan
if (-not $ImagePath -and -not $NonInteractive) {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $picker = New-Object System.Windows.Forms.OpenFileDialog
        $picker.Title = '选择双击打不开、但右键指定画图能打开的同一张图片（不会读取图片内容）'
        $picker.Filter = '图片|*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp;*.heic;*.avif|所有文件|*.*'
        try { if ($picker.ShowDialog() -eq 'OK') { $ImagePath = $picker.FileName } }
        finally { $picker.Dispose() }
    } catch { Warn "File picker unavailable: $($_.Exception.Message)" }
}
if ($ImagePath) {
    $ext = [IO.Path]::GetExtension($ImagePath)
    if ($ext -match '^\.[A-Za-z0-9]{1,15}$') { $Extension = $ext.ToLowerInvariant() }
}
if (-not $DataRoot) { $DataRoot = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Pulse' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot ('Reports\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }

$report = [ordered]@{
    SchemaVersion = 1; CollectedUtc = [DateTime]::UtcNow.ToString('o'); LookbackMinutes = $Minutes
    Scope = 'Read-only metadata; no image contents, no default-app changes, no picture launches, no uploads.'
    Feedback = [ordered]@{ Failure = '未确认'; RightClickPaint = '未确认'; OtherViewDoubleClick = '未确认'; Note = '' }
}
if (-not $NonInteractive) {
    $report.Feedback.Failure = Choose '在 Pulse 的“详细信息”文件列表里双击，是什么表现？' @('完全没反应', '画图启动但图片没加载', '出现错误提示', '尚未复现')
    $report.Feedback.RightClickPaint = Choose '同一张图片，右键“打开方式”选画图能打开吗？' @('能打开', '不能打开', '未测试')
    $report.Feedback.OtherViewDoubleClick = Choose '切换为“大图标”等其他视图后，双击同一张图片能打开吗？' @('能打开，只有详细信息失败', '也打不开', '未测试')
    $report.Feedback.Note = Protect-Text (Read-Host '备注：可填写复现时间/错误码；可直接回车，请勿填写敏感信息')
}
$report.OS = [ordered]@{
    ProductName = Read-Registry 'LocalMachine' 'SOFTWARE\Microsoft\Windows NT\CurrentVersion' 'ProductName'
    DisplayVersion = Read-Registry 'LocalMachine' 'SOFTWARE\Microsoft\Windows NT\CurrentVersion' 'DisplayVersion'
    Build = Read-Registry 'LocalMachine' 'SOFTWARE\Microsoft\Windows NT\CurrentVersion' 'CurrentBuild'
    UBR = Read-Registry 'LocalMachine' 'SOFTWARE\Microsoft\Windows NT\CurrentVersion' 'UBR'
    Is64BitOperatingSystem = [Environment]::Is64BitOperatingSystem
    Is64BitCollector = [Environment]::Is64BitProcess; PowerShell = $PSVersionTable.PSVersion.ToString()
    Apartment = [Threading.Thread]::CurrentThread.ApartmentState.ToString()
    LongPathsEnabled = Read-Registry 'LocalMachine' 'SYSTEM\CurrentControlSet\Control\FileSystem' 'LongPathsEnabled'
}
$report.Image = [ordered]@{ Extension = $Extension; PathProvided = [bool]$ImagePath }
if ($ImagePath) {
    $report.Image.PathCharacters = $ImagePath.Length
    $report.Image.IsUNC = ($ImagePath.StartsWith('\\') -and -not $ImagePath.StartsWith('\\?\') -and -not $ImagePath.StartsWith('\\.\')) -or $ImagePath.StartsWith('\\?\UNC\', [StringComparison]::OrdinalIgnoreCase)
    $report.Image.HasExtendedPrefix = $ImagePath.StartsWith('\\?\')
    $report.Image.ContainsSpaces = $ImagePath.Contains(' ')
    $report.Image.ContainsNonAscii = [regex]::IsMatch($ImagePath, '[^\x00-\x7F]')
    try {
        $item = Get-Item -LiteralPath $ImagePath
        $report.Image.Exists = $true; $report.Image.SizeBytes = $item.Length
        $report.Image.Attributes = $item.Attributes.ToString(); $report.Image.IsDirectory = [bool]$item.PSIsContainer
    } catch { $report.Image.Exists = $false; $report.Image.Error = Protect-Text $_.Exception.Message }
}

$nativeReady = $false
try {
    if (-not ('PulseImageDiag.Native' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
namespace PulseImageDiag {
    public sealed class AssociationResult {
        public string HResult; public bool Success; public string Value;
    }
    public sealed class ProcessState {
        public int ProcessId; public bool ElevationKnown; public bool Elevated;
        public bool ArchitectureKnown; public bool Wow64; public int Win32Error;
    }
    public static class Native {
        [DllImport("shlwapi.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
        private static extern int AssocQueryStringW(uint flags, uint kind, string assoc,
            string verb, StringBuilder value, ref uint count);
        [DllImport("kernel32.dll", SetLastError=true)]
        private static extern IntPtr OpenProcess(uint access, bool inherit, int id);
        [DllImport("advapi32.dll", SetLastError=true)]
        private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
        [DllImport("advapi32.dll", SetLastError=true)]
        private static extern bool GetTokenInformation(IntPtr token, int kind, out int value, int size, out int needed);
        [DllImport("kernel32.dll", SetLastError=true)]
        private static extern bool IsWow64Process(IntPtr process, out bool wow64);
        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr handle);
        public static AssociationResult Query(string assoc, string verb, uint kind) {
            uint count = 32768; var value = new StringBuilder((int)count);
            int hr = AssocQueryStringW(0, kind, assoc, verb, value, ref count);
            return new AssociationResult { HResult = String.Format("0x{0:X8}", unchecked((uint)hr)),
                Success = hr == 0, Value = hr == 0 ? value.ToString() : null };
        }
        public static ProcessState InspectProcess(int id) {
            var result = new ProcessState { ProcessId = id };
            IntPtr process = OpenProcess(0x1000, false, id), token = IntPtr.Zero;
            if (process == IntPtr.Zero) { result.Win32Error = Marshal.GetLastWin32Error(); return result; }
            try {
                bool wow64;
                result.ArchitectureKnown = IsWow64Process(process, out wow64); result.Wow64 = wow64;
                if (!OpenProcessToken(process, 8, out token)) { result.Win32Error = Marshal.GetLastWin32Error(); return result; }
                int elevation, needed;
                if (GetTokenInformation(token, 20, out elevation, 4, out needed)) {
                    result.ElevationKnown = true; result.Elevated = elevation != 0;
                } else { result.Win32Error = Marshal.GetLastWin32Error(); }
                return result;
            } finally { if (token != IntPtr.Zero) CloseHandle(token); CloseHandle(process); }
        }
    }
}
'@
    }
    $nativeReady = $true
    $report.CollectorProcess = [PulseImageDiag.Native]::InspectProcess($PID)
} catch { $nativeReady = $false; Warn "Native read-only APIs unavailable: $($_.Exception.Message)" }

$report.PulseProcesses = @()
foreach ($p in @(Get-Process -Name pulse -ErrorAction SilentlyContinue | Select-Object -First 10)) {
    $entry = [ordered]@{ ProcessId = $p.Id }
    try {
        $entry.Executable = Protect-Text $p.Path
        if ($p.Path) {
            $v = [Diagnostics.FileVersionInfo]::GetVersionInfo($p.Path)
            $entry.FileVersion = $v.FileVersion; $entry.ProductVersion = $v.ProductVersion
            if (-not $p.Path.StartsWith('\\')) {
                try { $entry.ExecutableSHA256 = (Get-FileHash -LiteralPath $p.Path -Algorithm SHA256).Hash }
                catch { $entry.HashError = Protect-Text $_.Exception.Message }
            }
        }
    } catch { $entry.VersionError = Protect-Text $_.Exception.Message }
    if ($nativeReady) { $entry.Token = [PulseImageDiag.Native]::InspectProcess($p.Id) }
    $report.PulseProcesses += [pscustomobject]$entry
}
$report.Packages = @()
if (Get-Command Get-AppxPackage -ErrorAction SilentlyContinue) {
    foreach ($name in @('Microsoft.Paint', 'Microsoft.Windows.Photos')) {
        try {
            foreach ($package in @(Get-AppxPackage -Name $name -ErrorAction Stop)) {
                $entry = [ordered]@{ Name = $package.Name; Version = $package.Version.ToString(); Family = $package.PackageFamilyName; Status = $package.Status.ToString(); Aumids = @() }
                try {
                    $manifest = Get-AppxPackageManifest -Package $package.PackageFullName -ErrorAction Stop
                    foreach ($app in @($manifest.Package.Applications.Application)) {
                        if ($app.Id) { $entry.Aumids += $package.PackageFamilyName + '!' + $app.Id }
                    }
                } catch { $entry.ManifestError = Protect-Text $_.Exception.Message }
                $report.Packages += [pscustomobject]$entry
            }
        } catch { Warn "Package read $name : $($_.Exception.Message)" }
    }
} else { Warn 'Get-AppxPackage is unavailable; classic Paint can still be valid.' }
$paintExe = Join-Path $env:WINDIR 'System32\mspaint.exe'
$report.LegacyPaint = [ordered]@{ Exists = Test-Path -LiteralPath $paintExe }
if ($report.LegacyPaint.Exists) {
    try { $report.LegacyPaint.FileVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($paintExe).FileVersion }
    catch { Warn "Paint executable version: $($_.Exception.Message)" }
}

$report.Registry = @(); $progIds = @()
foreach ($view in @('Registry64', 'Registry32')) {
    if ($view -eq 'Registry64' -and -not [Environment]::Is64BitOperatingSystem) { continue }
    $choicePath = 'Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\' + $Extension + '\UserChoice'
    $choice = Read-Registry 'CurrentUser' $choicePath 'ProgId' $view
    $systemDefault = Read-Registry 'ClassesRoot' $Extension '' $view
    $state = [ordered]@{ View = $view; UserChoiceProgId = $choice; UserChoiceHashPresent = Has-RegistryValue 'CurrentUser' $choicePath 'Hash' $view; ClassesRootDefault = $systemDefault; Classes = @() }
    foreach ($id in @($choice, $systemDefault, 'Paint.Picture', 'Paint.Picture.1', 'Applications\mspaint.exe') | Where-Object { $_ } | Select-Object -Unique) {
        $state.Classes += Class-Snapshot $id $view
    }
    if ($choice) { $progIds += $choice }; if ($systemDefault) { $progIds += $systemDefault }
    $report.Registry += [pscustomobject]$state
}
$report.AssocQueries = @()
if ($nativeReady) {
    $kinds = [ordered]@{ Command = 1; Executable = 2; FriendlyApp = 4; DelegateExecute = 18; ProgId = 20; AppId = 21 }
    foreach ($target in @($Extension) + @($progIds | Select-Object -Unique)) {
        foreach ($verb in @([pscustomobject]@{ Name = 'default'; Value = $null }, [pscustomobject]@{ Name = 'open'; Value = 'open' }, [pscustomobject]@{ Name = 'edit'; Value = 'edit' })) {
            foreach ($kind in $kinds.GetEnumerator()) {
                try {
                    $value = [PulseImageDiag.Native]::Query($target, $verb.Value, [uint32]$kind.Value)
                    $report.AssocQueries += [pscustomobject][ordered]@{ Target = $target; Verb = $verb.Name; Kind = $kind.Key; HResult = $value.HResult; Success = $value.Success; Value = Protect-Text $value.Value }
                } catch { Warn "Association query $target/$($verb.Name)/$($kind.Key): $($_.Exception.Message)" }
            }
        }
    }
}

$report.Runtime = [ordered]@{ DirectoryExists = $false; FilesExamined = 0; Events = @(); InstrumentedOpenEvents = $false }
$logRoot = Join-Path $DataRoot 'Diagnostics\Runtime'
$allowedEvents = @('process_start', 'file_double_click', 'shell_open_queued', 'shell_open_begin', 'shell_open_result', 'shell_command_result')
$allowedFields = @('row_hit', 'details_view', 'column_layout', 'has_index', 'item_index', 'task', 'default_verb', 'explicit_app', 'path_chars', 'queue_size', 'queue_ms', 'com_hresult', 'error', 'cancelled', 'process_id', 'shell_mask', 'elapsed_ms', 'create_error', 'elevation_requested', 'pointer_bits', 'os_known', 'os_major', 'os_minor', 'os_build')
if (Test-Path -LiteralPath $logRoot -PathType Container) {
    $report.Runtime.DirectoryExists = $true
    foreach ($log in @(Get-ChildItem -LiteralPath $logRoot -Filter '*.jsonl*' -File | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 8)) {
        $report.Runtime.FilesExamined++
        try {
            foreach ($line in @(Get-Content -LiteralPath $log.FullName -Tail 2500 -Encoding UTF8)) {
                try {
                    $event = $line | ConvertFrom-Json
                    if ($allowedEvents -notcontains $event.event -or [DateTime]::Parse($event.utc).ToUniversalTime() -lt $startUtc) { continue }
                    $fields = [ordered]@{}
                    foreach ($property in $event.data.PSObject.Properties) {
                        if ($allowedFields -contains $property.Name -and $property.Value -is [ValueType]) { $fields[$property.Name] = $property.Value }
                    }
                    $report.Runtime.Events += [pscustomobject][ordered]@{ Utc = $event.utc; Session = $event.session; ProcessId = $event.pid; Component = $event.component; Version = $event.version; Build = $event.build; Event = $event.event; Data = $fields }
                    if ($event.event -eq 'shell_open_result') { $report.Runtime.InstrumentedOpenEvents = $true }
                } catch { }
            }
        } catch { Warn "Runtime log $($log.Name): $($_.Exception.Message)" }
    }
    $report.Runtime.Events = @($report.Runtime.Events | Sort-Object Utc | Select-Object -Last 500)
}

$report.ActivationEvents = @()
if (-not $SkipEventLogs) {
    $terms = @('mspaint', 'Microsoft\.Paint', 'Microsoft\.Windows\.Photos') + @($progIds | Select-Object -Unique | ForEach-Object { [regex]::Escape($_) })
    $pattern = $terms -join '|'
    foreach ($logName in @('Microsoft-Windows-AppModel-Runtime/Admin', 'Microsoft-Windows-TWinUI/Operational', 'Application')) {
        try {
            foreach ($event in @(Get-WinEvent -FilterHashtable @{ LogName = $logName; StartTime = $startUtc.ToLocalTime() } -MaxEvents 100 -ErrorAction Stop)) {
                if ($event.Message -notmatch $pattern) { continue }
                $text = Protect-Text $event.Message
                if ($text.Length -gt 3000) { $text = $text.Substring(0, 3000) + ' [truncated]' }
                $report.ActivationEvents += [pscustomobject][ordered]@{ Utc = $event.TimeCreated.ToUniversalTime().ToString('o'); Log = $logName; EventId = $event.Id; Provider = $event.ProviderName; Level = $event.LevelDisplayName; Message = $text }
            }
        } catch {
            if ($_.FullyQualifiedErrorId -notlike 'NoMatchingEventsFound*') { Warn "Event log ${logName}: $($_.Exception.Message)" }
        }
    }
}
$report.Warnings = @($warnings.ToArray())
$report.Caution = @(
    'Shell success means activation was accepted, not proof that a picture appeared in an application.',
    'No instrumented open events can mean an older build, no recent reproduction, a different account/data root, or a collector time window mismatch.',
    'Registry/API differences alone do not prove a damaged UserChoice. Do not rewrite UserChoice or its Hash.',
    'Compare the reported failure time, same image, same process and view before attributing an error to this case.'
)

[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$jsonFile = Join-Path $OutputDirectory 'report.json'
$summaryFile = Join-Path $OutputDirectory 'summary.txt'
$utf8 = New-Object System.Text.UTF8Encoding($true)
[IO.File]::WriteAllText($jsonFile, ($report | ConvertTo-Json -Depth 30), $utf8)
$summary = @(
    'Pulse 图片打开诊断',
    '==================',
    ('采集时间 UTC: ' + $report.CollectedUtc),
    ('系统 Build/UBR: ' + $report.OS.Build + '.' + $report.OS.UBR + ' / ' + $report.OS.DisplayVersion),
    ('图片扩展名: ' + $Extension + '；不包含图片内容或完整图片路径'),
    ('故障表现: ' + $report.Feedback.Failure),
    ('右键指定画图: ' + $report.Feedback.RightClickPaint),
    ('其他视图双击: ' + $report.Feedback.OtherViewDoubleClick),
    ('Pulse 进程数: ' + $report.PulseProcesses.Count),
    ('相关运行事件: ' + $report.Runtime.Events.Count + '；新版打开结果日志: ' + $report.Runtime.InstrumentedOpenEvents),
    ('相关 Windows 激活事件: ' + $report.ActivationEvents.Count),
    ('采集警告: ' + $warnings.Count),
    '',
    '请把 report.json、summary.txt 和复现时间交给开发者。发送前可以自行检查报告。',
    '未修改默认应用、注册表或应用权限，没有上传、启动图片查看器或读取图片内容。',
    '只有健康电脑的信息不能确定故障电脑的根因；没有日志也不等于没有触发双击。'
)
[IO.File]::WriteAllLines($summaryFile, $summary, $utf8)
Write-Host "`n诊断完成。报告位置：$OutputDirectory" -ForegroundColor Green
Write-Host '请回传 report.json、summary.txt。无需发送原始图片。'
