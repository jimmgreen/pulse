[CmdletBinding()]
param([string]$ScriptPath)
$ErrorActionPreference = 'Stop'
if (-not $ScriptPath) { $ScriptPath = Join-Path $PSScriptRoot '..\tools\diagnose_index_storage.ps1' }
$ScriptPath = [IO.Path]::GetFullPath($ScriptPath)
$failures = 0
function Check($ok, [string]$name) {
    if ($ok) { Write-Output ('[PASS] ' + $name) } else { Write-Output ('[FAIL] ' + $name); $script:failures++ }
}
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref]$tokens, [ref]$errors)
Check ($errors.Count -eq 0) 'PowerShell AST syntax'
if ($errors.Count) { $errors | ForEach-Object { Write-Output $_.Message }; exit 1 }
$source = [IO.File]::ReadAllText($ScriptPath)
$native = [regex]::Match($source, "(?s)Add-Type -TypeDefinition @'\r?\n(.*?)\r?\n'@")
Check $native.Success 'native metadata probe is found as one bounded source block'
if ($native.Success) { Add-Type -TypeDefinition $native.Groups[1].Value; Check $true 'C# native helper compiles without probing a volume' }
$parent = Join-Path (Get-Location).Path 'bench_data'
[IO.Directory]::CreateDirectory($parent) | Out-Null
$root = Join-Path $parent ('index-diag-fixture-' + [Guid]::NewGuid().ToString('N'))
if ([IO.Directory]::Exists($root)) { throw 'Exclusive fixture already exists.' }
[IO.Directory]::CreateDirectory($root) | Out-Null
$oldTemp = $env:TEMP; $oldTmp = $env:TMP
$env:TEMP = $root; $env:TMP = $root
try {
    # Load only the log sanitizer; invoking the full script must keep system probes disabled.
    $eventFunction = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'SafeRuntimeEvents' }, $true)
    . ([scriptblock]::Create($eventFunction.Extent.Text))
    $logRoot = Join-Path $root 'fake-runtime'
    [IO.Directory]::CreateDirectory($logRoot) | Out-Null
    $logPaths = @()
    foreach ($age in 0..3) {
        $logPath = Join-Path $logRoot ('index-fixture-' + $age + '.jsonl')
        $logPaths += $logPath
        $version = '1.0.' + (54 - $age)
        $lines = New-Object 'System.Collections.Generic.List[string]'
        # Three selected files exceed 240 events; the fourth must never be included.
        foreach ($scan in 1..24) {
            foreach ($eventName in 'index_rebuild_begin','index_volume_scan_failed','index_rebuild_end') {
                $lines.Add((@{ event=$eventName; version=$version; data=@{ drive=68; phase=3; records=123456; limit=250000; error=30; scan=$scan; path='C:\PRIVATE-LOG-PATH-SENTINEL'; nested=@{ secret='PRIVATE-NESTED-SENTINEL' } } } | ConvertTo-Json -Compress))
            }
        }
        foreach ($heartbeat in 1..200) {
            $lines.Add((@{ event='process_health'; version=$version; data=@{ heartbeat=$heartbeat } } | ConvertTo-Json -Compress))
        }
        $lines.Add('{malformed json')
        $lines.Add('{"event":"PRIVATE-UNKNOWN-EVENT-SENTINEL","data":{"error":999}}')
        [IO.File]::WriteAllLines($logPath, $lines, (New-Object Text.UTF8Encoding($false)))
        [IO.File]::SetLastWriteTimeUtc($logPath, [DateTime]::UtcNow.AddHours(-$age))
    }
    $logHashesBefore = @($logPaths | ForEach-Object { (Get-FileHash -LiteralPath $_).Hash }) -join ','
    $events = @(SafeRuntimeEvents $logRoot)
    Check ($events.Count -eq 240) 'three log files retain at most 240 events despite repeated rebuilds and heartbeats'
    Check ($events[0].version -eq '1.0.54' -and $events[79].version -eq '1.0.54' -and $events[80].version -eq '1.0.53' -and $events[239].version -eq '1.0.52' -and @($events | Where-Object { $_.version -eq '1.0.51' }).Count -eq 0) 'newest files appear first and old versions cannot displace their reserved evidence'
    $failure = @($events | Where-Object { $_.version -eq '1.0.54' -and $_.event -eq 'index_volume_scan_failed' -and $_.data.scan -eq 24 })
    Check ($failure.Count -eq 1 -and $failure[0].data.drive -eq 68 -and $failure[0].data.phase -eq 3 -and $failure[0].data.records -eq 123456 -and $failure[0].data.limit -eq 250000 -and $failure[0].data.error -eq 30) 'scan failure numeric fields survive more than 160 later heartbeats'
    Check (@($events | Where-Object { $_.version -eq '1.0.54' -and $_.data.scan -eq 24 -and $_.event -in 'index_rebuild_begin','index_rebuild_end' }).Count -eq 2) 'rebuild begin and end survive later health events'
    Check (@($events | Where-Object { $_.version -eq '1.0.54' -and $_.event -eq 'process_health' -and $_.data.heartbeat -eq 200 }).Count -eq 1) 'latest health sample is retained alongside earlier failures'
    $safeJson = $events | ConvertTo-Json -Depth 12
    Check (-not $safeJson.Contains('PRIVATE-') -and -not $safeJson.Contains('"path"') -and -not $safeJson.Contains('"nested"')) 'unknown events, path strings and nested private fields are excluded'
    Check ((@($logPaths | ForEach-Object { (Get-FileHash -LiteralPath $_).Hash }) -join ',') -eq $logHashesBefore) 'log fixture contents remain unchanged'
    $firstScanRoot = Join-Path $root 'fake-first-scan'
    [IO.Directory]::CreateDirectory($firstScanRoot) | Out-Null
    $firstScanLines = @(
        '{"event":"index_rebuild_begin","version":"1.0.54","data":{}}'
        '{"event":"index_volume_scan_failed","version":"1.0.54","data":{"drive":68,"phase":3,"error":30}}'
        '{"event":"index_rebuild_end","version":"1.0.54","data":{"error":30}}'
    ) + @(1..200 | ForEach-Object { '{"event":"process_health","version":"1.0.54","data":{"alive":1}}' })
    [IO.File]::WriteAllLines((Join-Path $firstScanRoot 'index-first.jsonl'), $firstScanLines)
    $firstScanEvents = @(SafeRuntimeEvents $firstScanRoot)
    Check ($firstScanEvents.Count -eq 19 -and $firstScanEvents[0].event -eq 'index_rebuild_begin' -and $firstScanEvents[1].event -eq 'index_volume_scan_failed' -and $firstScanEvents[2].event -eq 'index_rebuild_end') 'first scan failure and rebuild boundaries survive a long idle process'
    $unicodeName = [string][char]0x7d22 + [string][char]0x5f15 + ' test'
    $directory = Join-Path $root $unicodeName
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $privateName = 'PRIVATE-CONTENT-SENTINEL.txt'
    $privateFile = Join-Path $directory $privateName
    [IO.File]::WriteAllText($privateFile, 'PRIVATE-DOCUMENT-CONTENT-DO-NOT-COLLECT')
    $configuration = Join-Path $root 'fixture-config.json'
    $value = @{ version=1; generation=123; index_path=$directory; excluded_volume_ids=@(); excluded_paths=@('PRIVATE-EXCLUDED-PATH-SENTINEL') }
    [IO.File]::WriteAllText($configuration, ($value | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    $before = [IO.File]::ReadAllText($privateFile)
    $output = @(& $ScriptPath -ConfigurationFile $configuration -InstallDirectory $root -OutputDirectory $root -SkipSystemProbes -SkipLogs)
    $line = @($output | Where-Object { $_ -like 'REPORT_FILE=*' })[0]
    Check ($null -ne $line) 'diagnosis creates a uniquely named report'
    $file = $line.Substring(12)
    $text = [IO.File]::ReadAllText($file)
    $report = $text | ConvertFrom-Json
    Check ($report.configuration.found -and $report.configuration.generation -eq 123) 'reads only requested fake configuration'
    Check ($report.index_directory.exists -and $report.index_directory.shape.has_non_ascii) 'Unicode and spaces survive local metadata inspection'
    Check (-not $report.service.checked -and @($report.volumes).Count -eq 0 -and @($report.runtime_events).Count -eq 0) 'isolated test never reads real service, disks or logs'
    Check (-not $text.Contains($directory) -and -not $text.Contains($privateName) -and -not $text.Contains('PRIVATE-DOCUMENT-CONTENT') -and -not $text.Contains('PRIVATE-EXCLUDED-PATH')) 'no folder names, document contents or excluded private paths leak'
    Check ([IO.File]::ReadAllText($privateFile) -eq $before -and [IO.File]::ReadAllText($configuration).Contains('PRIVATE-EXCLUDED-PATH-SENTINEL')) 'input files and fake configuration remain unchanged'
    $second = @(& $ScriptPath -IndexDirectory '\\DO-NOT-CONNECT-SENTINEL\private-share' -OutputDirectory $root -SkipSystemProbes -SkipLogs)
    $secondPath = (@($second | Where-Object { $_ -like 'REPORT_FILE=*' })[0]).Substring(12)
    $secondText = [IO.File]::ReadAllText($secondPath)
    $secondReport = $secondText | ConvertFrom-Json
    Check ($secondReport.index_directory.blocked -eq 'not-a-local-absolute-drive-path' -and -not $secondText.Contains('DO-NOT-CONNECT-SENTINEL')) 'UNC path is blocked before any share access and redacted'
    $third = @(& $ScriptPath -IndexDirectory $directory -OutputDirectory $root -ProbeVolumes -SkipSystemProbes -SkipLogs)
    $thirdPath = (@($third | Where-Object { $_ -like 'REPORT_FILE=*' })[0]).Substring(12)
    $thirdReport = [IO.File]::ReadAllText($thirdPath) | ConvertFrom-Json
    Check ($null -eq $thirdReport.raw_metadata_probes -and @($thirdReport.notes).Count -gt 0) 'native probe cannot run in explicitly isolated mode'
    Check ($file -ne $secondPath -and $file -ne $thirdPath -and $secondPath -ne $thirdPath) 'later reports do not overwrite earlier reports'
    $nativeDir = Join-Path $root ($unicodeName + ' native launcher')
    [IO.Directory]::CreateDirectory($nativeDir) | Out-Null
    Copy-Item -LiteralPath $ScriptPath -Destination (Join-Path $nativeDir 'diagnose_index_storage.ps1')
    Copy-Item -LiteralPath $configuration -Destination (Join-Path $nativeDir 'fixture-config.json')
    $bad = Join-Path $nativeDir 'bad-launcher.cmd'
    $good = Join-Path $nativeDir 'fixed-launcher.cmd'
    $base = '@echo off' + "`r`n" + 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0diagnose_index_storage.ps1" -ConfigurationFile "%~dp0fixture-config.json" -SkipSystemProbes -SkipLogs'
    [IO.File]::WriteAllText($bad, $base + ' -OutputDirectory "%~dp0"' + "`r`nexit /b %ERRORLEVEL%`r`n", [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText($good, $base + "`r`nexit /b %ERRORLEVEL%`r`n", [Text.Encoding]::ASCII)
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $badOutput = @(& $bad 2>&1)
    $badCode = $LASTEXITCODE
    $goodOutput = @(& $good 2>&1)
    $goodCode = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    Check ($badCode -ne 0) 'reproduce quoted trailing-backslash argument failure through real cmd and PowerShell processes'
    Check ($goodCode -eq 0) 'fixed native launcher exits successfully with Unicode and spaces'
    if ($goodCode -ne 0) { $goodOutput | ForEach-Object { Write-Output ([string]$_) } }
    $nativeReports = @(Get-ChildItem -LiteralPath $nativeDir -Filter 'PulseIndexReport-*.json' -File)
    Check ($nativeReports.Count -eq 1 -and (([IO.File]::ReadAllText($nativeReports[0].FullName) | ConvertFrom-Json).diagnostic_version -eq '1.2')) 'fixed native launcher creates exactly one version 1.2 report'

} finally {
    $env:TEMP = $oldTemp; $env:TMP = $oldTmp
    if ([IO.Path]::GetDirectoryName($root) -ne $parent -or -not [IO.Path]::GetFileName($root).StartsWith('index-diag-fixture-')) { throw 'Fixture cleanup guard failed.' }
    [IO.Directory]::Delete($root, $true)
}
if ($failures) { exit 1 }
Write-Output 'All isolated diagnostic checks passed; no production index or service was touched.'
