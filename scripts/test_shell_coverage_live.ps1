# Real Shell coverage; snapshots/restores registration and uses an isolated profile.
# Shared safety helpers are copied from test_shell_entry_live.ps1; that script is not executed.
param(
 [string]$Pulse = (Join-Path $PSScriptRoot '..\build\pulse.exe'),
 [string]$Driver = (Join-Path $PSScriptRoot '..\bench_data\shell_entry_live_driver.exe'),
 [string]$Launcher = (Join-Path $PSScriptRoot '..\build\pulse_launcher.exe'),
 [switch]$WarmOnly
)
$ErrorActionPreference = 'Stop'
$Pulse = (Resolve-Path -LiteralPath $Pulse).Path
$Driver = (Resolve-Path -LiteralPath $Driver).Path
$repo = Split-Path $PSScriptRoot -Parent
$run = Join-Path $repo ('bench_data\shell-entry-live-coverage-' + [guid]::NewGuid().ToString())
[void][IO.Directory]::CreateDirectory($run)
$report = Join-Path $run 'results.txt'
function Log([string]$Text) { $Text | Tee-Object -FilePath $report -Append }
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class ShellEntryWindows {
 public delegate bool Callback(IntPtr h, IntPtr p);
 [DllImport("user32.dll")] static extern bool EnumWindows(Callback c, IntPtr p);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int command);
 [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int key);
 [DllImport("user32.dll")] public static extern bool EnableWindow(IntPtr h, bool enabled);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 public static long[] Pulse(uint pid) {
  var result=new List<long>();
  EnumWindows((h,p)=>{uint owner;GetWindowThreadProcessId(h,out owner);var s=new StringBuilder(256);GetClassName(h,s,256);
   if(s.ToString()=="PulseMainWindow" && (pid==0 || owner==pid))result.Add(h.ToInt64());return true;},IntPtr.Zero);
  return result.ToArray();
 }
}
'@
if (@([ShellEntryWindows]::Pulse(0)).Count) { throw 'A preexisting Pulse window exists; refusing to disturb the user instance.' }
$keys = @('Software\Classes\Directory','Software\Classes\Drive',
 'Software\Classes\CLSID\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}',
 'Software\Classes\CLSID\{20D04FE0-3AEA-1069-A2D8-08002B30309D}',
 'Software\Classes\CLSID\{645FF040-5081-101B-9F08-00AA002F954E}', 'Software\Pulse')
$hive = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
function Read-Tree([string]$Path) {
 $key=$hive.OpenSubKey($Path)
 if (!$key) { return }
 try {
  $values=@(foreach($name in ($key.GetValueNames() | Sort-Object)) {
   [pscustomobject]@{Name=$name;Kind=[int]$key.GetValueKind($name);Data=$key.GetValue($name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}
  })
  [pscustomobject]@{Path=$Path;Values=$values}
  foreach($child in ($key.GetSubKeyNames() | Sort-Object)) { Read-Tree ($Path+'\'+$child) }
 } finally { $key.Dispose() }
}
function Snapshot { @(foreach($key in $keys) { Read-Tree $key }) }
function Canonical($Snapshot) { ConvertTo-Json -InputObject @($Snapshot) -Depth 30 -Compress }
$before=Snapshot
$before | Export-Clixml -LiteralPath (Join-Path $run 'registry-before.clixml') -Depth 30
$beforeText=Canonical $before
[IO.File]::WriteAllText((Join-Path $run 'registry-before.json'),$beforeText)
function Restore-Registry {
 $original=@{};foreach($item in $before){$original[$item.Path]=$item}
 $expected=@{};foreach($item in $script:expectedRegistry){$expected[$item.Path]=$item}
 $current=@(Snapshot)
 foreach($item in $current) {
  $oldValues=@{};if($original.ContainsKey($item.Path)){foreach($v in $original[$item.Path].Values){$oldValues[$v.Name]=$v}}
  $expectedValues=@{};if($expected.ContainsKey($item.Path)){foreach($v in $expected[$item.Path].Values){$expectedValues[$v.Name]=$v}}
  $key=$hive.OpenSubKey($item.Path,$true)
  try {foreach($v in $item.Values) {
   if($oldValues.ContainsKey($v.Name) -and (Canonical $v) -ceq (Canonical $oldValues[$v.Name])){continue}
   if(!$expectedValues.ContainsKey($v.Name) -or (Canonical $v) -cne (Canonical $expectedValues[$v.Name])) {
    Log "[RESTORE-CONFLICT] Preserving externally changed value $($item.Path) / $($v.Name)";continue
   }
   if($oldValues.ContainsKey($v.Name)){$old=$oldValues[$v.Name];$key.SetValue($old.Name,$old.Data,[Microsoft.Win32.RegistryValueKind]$old.Kind)}
   else{$key.DeleteValue($v.Name,$false)}
  }}finally{$key.Dispose()}
 }
 foreach($item in $before) {
  $key=$hive.OpenSubKey($item.Path,$true)
  if(!$key){
   if($expected.ContainsKey($item.Path)){Log "[RESTORE-CONFLICT] Preserving external removal of $($item.Path)";continue}
   $key=$hive.CreateSubKey($item.Path)
  }
  try {foreach($v in $item.Values){
   if($key.GetValueNames() -contains $v.Name){continue}
   $expectedPresent=$expected.ContainsKey($item.Path) -and @($expected[$item.Path].Values | Where-Object {$_.Name -eq $v.Name}).Count -gt 0
   if($expectedPresent){Log "[RESTORE-CONFLICT] Preserving external value removal $($item.Path) / $($v.Name)";continue}
   $key.SetValue($v.Name,$v.Data,[Microsoft.Win32.RegistryValueKind]$v.Kind)
  }}finally{$key.Dispose()}
 }
 # Never delete an original key (and its ACL). Remove only empty keys created
 # by this run, deepest first, leaving unknown external content untouched.
 foreach($item in (@(Snapshot) | Sort-Object {$_.Path.Length} -Descending)) {
  if($original.ContainsKey($item.Path) -or !$expected.ContainsKey($item.Path)){continue}
  $key=$hive.OpenSubKey($item.Path)
  $empty=$key -and $key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0
  if($key){$key.Dispose()}
  if($empty){$hive.DeleteSubKey($item.Path,$false)}
 }
}
$script:expectedRegistry=$before
$script:foldersEnabled=0
function User-Preferences {
 $root=Join-Path $env:LOCALAPPDATA 'Pulse'
 @(foreach($name in @('app.json','session.json','session-v2.json','places.json')) {
  $file=Join-Path $root $name
  [pscustomobject]@{Name=$name;Hash=$(if(Test-Path -LiteralPath $file){(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}else{'ABSENT'})}
 })
}
$prefsBefore=ConvertTo-Json -InputObject @(User-Preferences) -Compress
$preferenceBackup=Join-Path $run 'user-preferences-backup'
[void][IO.Directory]::CreateDirectory($preferenceBackup)
foreach($item in @(User-Preferences)) {
 if($item.Hash -ne 'ABSENT') {Copy-Item -LiteralPath (Join-Path (Join-Path $env:LOCALAPPDATA 'Pulse') $item.Name) -Destination (Join-Path $preferenceBackup $item.Name)}
}
[IO.File]::WriteAllText((Join-Path $preferenceBackup 'manifest.json'),$prefsBefore)
function Native-Windows {
 $shell=New-Object -ComObject Shell.Application
 try { @($shell.Windows() | ForEach-Object {
  try { if($_.FullName -match '\\explorer.exe$') { [pscustomobject]@{Hwnd=[long]$_.HWND;Path=[string]$_.Document.Folder.Self.Path;Com=$_} } } catch {}
 }) } finally { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($shell) }
}
$nativeBefore=@(Native-Windows)
$nativeBefore|Select-Object Hwnd,Path|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $run 'native-before.json')
$nativeOwned=[Collections.Generic.HashSet[long]]::new()
$failures=0
function Check([bool]$Ok,[string]$Name) {
 if($Ok){Log "[PASS] $Name"}else{Log "[FAIL] $Name";$script:failures++}
}
function Invoke-Driver([string[]]$Arguments,[switch]$AllowFailure) {
 if($Arguments[0] -eq 'wine') {
  $key=$hive.OpenSubKey('Software\Classes\CLSID\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\shell\opennewwindow\command')
  try {Log ('WIN-E-BEFORE command='+$(if($key){$key.GetValue('')}else{'ABSENT'}))}finally{if($key){$key.Dispose()}}
 }
 $output=& $Driver @Arguments 2>&1
 $output | ForEach-Object { Log ([string]$_) }
 if($Arguments[0] -eq 'apply'){$script:expectedRegistry=Snapshot;$script:expectedRegistry|Export-Clixml -LiteralPath (Join-Path $run 'registry-last-written.clixml') -Depth 30}
 if($Arguments[0] -eq 'apply' -and $Arguments[1] -eq 'folders' -and $LASTEXITCODE -eq 0){
  $script:foldersEnabled=if($Arguments[2] -eq 'on'){1}else{0}
  if($script:process){$state=State;Set-Mode $state.Mode $state.Compat $state.Enabled $state.PcMode}
  else{[IO.File]::WriteAllText((Join-Path $run 'live-control.txt'),"1 1 1 1 $script:foldersEnabled")}
 }
 if($LASTEXITCODE -ne 0) { if($AllowFailure){Check $false "API exit=$LASTEXITCODE $Arguments (continuing to collect real window evidence)"}else{throw "Driver failed: $Arguments"} }
}
function State {
 $file=Join-Path $run 'live-state.tsv'
 if(!(Test-Path -LiteralPath $file)){return $null}
 try {
  $lines=[IO.File]::ReadAllLines($file)
  $windows=@(foreach($line in $lines) { $p=$line.Split("`t"); if($p[0] -eq 'WINDOW') {
   [pscustomobject]@{Hwnd=[long]$p[1];Tabs=[int]$p[2];Pending=[long]$p[3];Applied=[long]$p[4];Path=$p[5]}
  }})
  $mode=($lines | Where-Object { $_.StartsWith("MODE`t") }).Split("`t")
  $wine=($lines | Where-Object { $_.StartsWith("WINE`t") }).Split("`t")
  $folders=($lines | Where-Object {$_.StartsWith("FOLDERS`t")}).Split("`t")
  $presented=[long](($lines | Where-Object { $_.StartsWith("PRESENTED`t") }).Split("`t")[1])
  [pscustomobject]@{Windows=$windows;Tabs=($windows|Measure-Object Tabs -Sum).Sum;Mode=[int]$mode[1];Compat=[int]$mode[2];Enabled=[int]$mode[3];PcMode=[int]$mode[4];WinEScope=[int]$wine[1];WinERegistered=[int]$wine[2];WinEActive=[int]$wine[3];Presented=$presented;Folders=[int]$folders[1];FolderRegistryRunning=[int]$folders[2]}
 } catch { return $null }
}
function Wait-For([scriptblock]$Condition,[int]$Seconds=20) {
 $end=[DateTime]::UtcNow.AddSeconds($Seconds)
 do { if(& $Condition){return $true}; Start-Sleep -Milliseconds 100 } while([DateTime]::UtcNow -lt $end)
 return $false
}
function WinE-Events {
 if(!$process){return @()}
 $directory=Join-Path $env:LOCALAPPDATA 'Pulse\Diagnostics\Runtime'
 @(foreach($file in @(Get-ChildItem -LiteralPath $directory -Filter ("*-"+$process.Id+'.jsonl') -ErrorAction SilentlyContinue)) {
  foreach($line in @(Get-Content -LiteralPath $file.FullName -ErrorAction SilentlyContinue)) {
   try{$event=$line|ConvertFrom-Json;if($event.pid -eq $process.Id -and $event.event -in @('win_e_chord','win_e_delivery')){$event}}catch{}
  }
 })
}
function Set-Mode([int]$Mode,[int]$Compat=0,[int]$Enabled=1,[int]$PcMode=1) {
 # A preceding failed activation case can leave our windows minimized. Restore
 # only an owned fixture window before preparing the next independent case.
 $current=State
 if($current -and $current.Windows.Count){[void][ShellEntryWindows]::ShowWindow([IntPtr]$current.Windows[0].Hwnd,9)}
 [IO.File]::WriteAllText((Join-Path $run 'live-control.txt'),"$Mode $Compat $Enabled $PcMode $script:foldersEnabled")
 if(!(Wait-For { $s=State; $s -and $s.Mode -eq $Mode -and $s.Compat -eq $Compat -and $s.Enabled -eq $Enabled -and $s.PcMode -eq $PcMode -and $s.Folders -eq $script:foldersEnabled -and ($script:foldersEnabled -eq 0 -or $s.FolderRegistryRunning -eq 1) } 10)) {throw 'Live mode not acknowledged by a real presented frame'}
}
function Presented-Path([string]$Path) {
 $s=State
 $expected=Normalize-Path $Path
 return $s -and @($s.Windows | Where-Object { $_.Hwnd -eq $s.Presented -and (Normalize-Path $_.Path) -ieq $expected -and $_.Pending -eq 0 -and $_.Applied -gt 0 }).Count -gt 0
}
function Normalize-Path([string]$Path) {
 if($Path.StartsWith('\\?\UNC\')){$Path='\\'+$Path.Substring(8)}
 elseif($Path.StartsWith('\\?\')){$Path=$Path.Substring(4)}
 return $Path.TrimEnd('\')
}
function Open-PulsePath([string]$Target,[string]$Expected,[string]$Label) {
 Invoke-Driver -Arguments @('shell',$Target)
 Check (Wait-For { Presented-Path $Expected }) "${Label}: true ShellExecute reaches loaded, presented Pulse target"
 Copy-Item -LiteralPath (Join-Path $run 'live-state.tsv') -Destination (Join-Path $run ($Label+'.tsv'))
}
function Open-Native([string]$Target,[string]$Expected,[string]$Label) {
 $old=@(Native-Windows).Hwnd
 $stateBefore=State
 Invoke-Driver -Arguments @('shell',$Target)
 $found=$null
 $ok=Wait-For { $script:found=@(Native-Windows | Where-Object { $_.Hwnd -notin $old -and $_.Path -ieq $Expected }) | Select-Object -First 1; $null -ne $script:found }
 Check $ok "${Label}: true ShellExecute remains in native Explorer"
 if($script:found){ [void]$nativeOwned.Add($script:found.Hwnd); $script:found.Com.Quit() }
 $stateAfter=State
 Check ($stateAfter.Tabs -eq $stateBefore.Tabs -and $stateAfter.Windows.Count -eq $stateBefore.Windows.Count) "${Label}: no extra Pulse page/window"
}
$process=$null
$disabledOwned=@()
$oldEnv=@{}
foreach($name in @('PULSE_TEST_DATA_DIR','PULSE_TEST_LIVE_SHELL_ENTRY','PULSE_TEST_WIN_E_HOTKEY')) { $oldEnv[$name]=[Environment]::GetEnvironmentVariable($name,'Process') }
function Stop-OwnedPulse {
 if(!$script:process){return}
 $parentId=$script:process.Id
 $indexPath=Join-Path (Split-Path $Pulse -Parent) 'Pulse.Index.exe'
 $children=@(Get-CimInstance Win32_Process -Filter "ParentProcessId=$parentId" | Where-Object {$_.ExecutablePath -ieq $indexPath -and $_.CommandLine -match '(?:^| )--network-agent(?: |$)'})
 $owned=@([ShellEntryWindows]::Pulse([uint32]$script:process.Id))
 if($owned.Count){[void][ShellEntryWindows]::PostMessage([IntPtr]$owned[0],0x8048,[IntPtr]::Zero,[IntPtr]::Zero)}
 if(!$script:process.WaitForExit(15000)){throw 'Owned Pulse did not exit; retaining process for cleanup'}
 foreach($child in $children){
  $current=Get-CimInstance Win32_Process -Filter ("ProcessId="+$child.ProcessId)
  if($current -and $current.CreationDate -eq $child.CreationDate -and $current.ParentProcessId -eq $parentId -and $current.ExecutablePath -ieq $indexPath){
   Stop-Process -Id $child.ProcessId -Force
   Log ("[CLEANUP] stopped fixture-owned network agent pid="+$child.ProcessId)
  }
 }
 $script:process=$null
}
function Start-OwnedPulse {
 Remove-Item -LiteralPath (Join-Path $run 'live-state.tsv') -ErrorAction SilentlyContinue
 $script:process=Start-Process -FilePath $Pulse -ArgumentList ('"'+(Join-Path $run 'baseline')+'"') -PassThru -WindowStyle Normal
 if(!(Wait-For {Presented-Path (Join-Path $run 'baseline')})){throw 'Pulse baseline did not present'}
}
function Capture-Case([string]$Label,[string]$Target,[string[]]$Arguments,[bool]$ExpectedCapture=$true) {
 $old=@(Native-Windows).Hwnd
 $began=[DateTime]::UtcNow
 Invoke-Driver -Arguments $Arguments -AllowFailure
 $captured=Wait-For {Presented-Path $Target} 12
 if($captured -and $ExpectedCapture){
  Check (Wait-For {@(Native-Windows|Where-Object {$_.Path -ieq $Target}).Count -eq 0} 5) "$Label native source closes after Pulse presentation"
 }
 $native=@(Native-Windows)
 $native|Select-Object Hwnd,Path|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $run ($Label+'-native.json'))
 foreach($window in $native){
  if($window.Hwnd -notin $nativeBefore.Hwnd -and $window.Hwnd -notin $old -and $window.Path -ieq $Target){[void]$nativeOwned.Add($window.Hwnd)}
 }
 $stateFile=Join-Path $run 'live-state.tsv'
 if(Test-Path -LiteralPath $stateFile){Copy-Item -LiteralPath $stateFile -Destination (Join-Path $run ($Label+'.tsv'))}
 if(Test-Path -LiteralPath $stateFile){
  $pidLine=[IO.File]::ReadAllLines($stateFile)|Where-Object {$_.StartsWith("PID`t")}
  if($pidLine){
   $probePid=[int]$pidLine.Split("`t")[1]
   $events=@(foreach($logFile in @(Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'Pulse\Diagnostics\Runtime') -Filter ("*-"+$probePid+'.jsonl') -ErrorAction SilentlyContinue)){
    foreach($line in @(Get-Content -LiteralPath $logFile.FullName)){
     try{$event=$line|ConvertFrom-Json;if([DateTime]::Parse($event.utc).ToUniversalTime() -ge $began -and $event.event -in @('external_open','explorer_candidate','explorer_handoff','explorer_candidate_link','shell_registry_bridge')){$line}}catch{}
    }
   })
   $events|Set-Content -LiteralPath (Join-Path $run ($Label+'-events.jsonl'))
  }
 }
 $targetNative=@($native|Where-Object {$_.Path -ieq $Target})
 Log "COVERAGE $Label captured=$captured native_target_windows=$($targetNative.Count)"
 if($captured -and $Label -match 'select') {
  $state=State
  $selected=@([IO.File]::ReadAllLines($stateFile)|Where-Object {$_ -eq ("SELECTED`t"+$state.Presented+"`tselected.txt")}).Count -gt 0
  Check $selected "$Label selected.txt is actually selected in the presented Pulse page"
 }
 if($ExpectedCapture){Check $captured "$Label real target loaded and presented in Pulse"}
 else{Log "[OBSERVED] $Label preexisting native reuse: captured=$captured (not counted as takeover success)";Check ($targetNative.Count -gt 0) "$Label preexisting native window remains available"}
 foreach($window in $native){if($nativeOwned.Contains($window.Hwnd) -and $window.Hwnd -notin $nativeBefore.Hwnd){$window.Com.Quit()}}
}
try {
 [void][IO.Directory]::CreateDirectory((Join-Path $run 'baseline'))
 $preferences=@{startup_open=1;home_folder=(Join-Path $run 'baseline');new_window_shows=1;external_open=1;open_this_pc=1;open_win_e=1;keep_running_on_close=$false;integration_enabled=$false;integration_folders=$false;integration_win_e=$false;integration_this_pc=$false;integration_recycle_bin=$false;integration_folders_compat=$false;take_over_explorer_windows=$false;auto_check_updates=$false}
 [IO.File]::WriteAllText((Join-Path $run 'app.json'),($preferences|ConvertTo-Json))
 [IO.File]::WriteAllText((Join-Path $run 'live-control.txt'),'1 1 1 1 0')
 $env:PULSE_TEST_DATA_DIR=$run;$env:PULSE_TEST_LIVE_SHELL_ENTRY='1';$env:PULSE_TEST_WIN_E_HOTKEY='1'
 Start-OwnedPulse
 $script:expectedRegistry=Snapshot
 Set-Mode 1 1 1
 Invoke-Driver -Arguments @('apply','folders','on',$Pulse)
 foreach($route in @('shell','native','native-select','select')) {
  $target=Join-Path $run ('warm-'+$route+' 中文 空格,目录')
  [void][IO.Directory]::CreateDirectory($target)
  $file=Join-Path $target 'selected.txt';[IO.File]::WriteAllText($file,'fixture')
  $arguments=switch($route){
   'shell' {@('shell',$target)}
   'native' {@('native-path',$target)}
   'native-select' {@('native-select',$file)}
   'select' {@('select',$file)}
  }
  Capture-Case ('warm-'+$route+' 中文 空格,目录') $target $arguments
 }
 # Contrast SH API behavior with verb registration disabled but the watcher active.
 Invoke-Driver -Arguments @('apply','folders','off',$Pulse)
 Set-Mode 1 1 1
 $target=Join-Path $run 'observer-only-select 中文 空格,目录'
 [void][IO.Directory]::CreateDirectory($target)
 $file=Join-Path $target 'selected.txt';[IO.File]::WriteAllText($file,'fixture')
 Capture-Case 'observer-only-select' $target @('select',$file)
 if(!$WarmOnly) {
 # A window that existed before observer activation must remain owned by Explorer.
 Set-Mode 1 0 0
 Invoke-Driver -Arguments @('apply','folders','off',$Pulse)
 $target=Join-Path $run 'preexisting-select';[void][IO.Directory]::CreateDirectory($target)
 $file=Join-Path $target 'selected.txt';[IO.File]::WriteAllText($file,'fixture')
 $old=@(Native-Windows).Hwnd
 Invoke-Driver -Arguments @('select',$file) -AllowFailure
 if(!(Wait-For {@(Native-Windows|Where-Object {$_.Hwnd -notin $old -and $_.Path -ieq $target}).Count -gt 0})){throw 'Could not establish owned native fixture'}
 foreach($window in @(Native-Windows|Where-Object {$_.Hwnd -notin $old -and $_.Path -ieq $target})){[void]$nativeOwned.Add($window.Hwnd)}
 Set-Mode 1 1 1
 Invoke-Driver -Arguments @('apply','folders','on',$Pulse)
 Capture-Case 'preexisting-select' $target @('select',$file) $false
 Stop-OwnedPulse
 $relay=Join-Path $run 'pulse_coverage_relay.exe'
 Copy-Item -LiteralPath $Driver -Destination $relay
 [IO.File]::WriteAllText((Join-Path $run 'relay.ini'),("[relay]`r`npulse="+$Pulse+"`r`n"),[Text.Encoding]::Unicode)
 Invoke-Driver -Arguments @('apply','folders','on',$relay)
 foreach($route in @('shell','native','native-select','select')) {
  $target=Join-Path $run ('cold-'+$route+' 中文 空格,目录');[void][IO.Directory]::CreateDirectory($target)
  $file=Join-Path $target 'selected.txt';[IO.File]::WriteAllText($file,'fixture')
  Remove-Item -LiteralPath (Join-Path $run 'live-state.tsv') -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath (Join-Path $run 'relay-pid.txt') -ErrorAction SilentlyContinue
  $arguments=switch($route){
   'shell' {@('shell',$target)}
   'native' {@('native-path',$target)}
   'native-select' {@('native-select',$file)}
   'select' {@('select',$file)}
  }
  Capture-Case ('cold-'+$route+' 中文 空格,目录') $target $arguments
  $pidFile=Join-Path $run 'relay-pid.txt'
  if(Test-Path -LiteralPath $pidFile){$script:process=Get-Process -Id ([int][IO.File]::ReadAllText($pidFile)) -ErrorAction SilentlyContinue}
  Stop-OwnedPulse
 }
 if(Test-Path -LiteralPath $Launcher){
  $broken=Join-Path $run 'broken-bundle';[void][IO.Directory]::CreateDirectory($broken)
  $brokenLauncher=Join-Path $broken 'pulse.exe';Copy-Item -LiteralPath $Launcher -Destination $brokenLauncher
  $target=Join-Path $run 'broken-launcher 中文 空格,目录';[void][IO.Directory]::CreateDirectory($target)
  Invoke-Driver -Arguments @('apply','folders','on',$brokenLauncher)
  $old=@(Native-Windows).Hwnd
  Invoke-Driver -Arguments @('shell',$target) -AllowFailure
  $found=Wait-For {@(Native-Windows|Where-Object {$_.Hwnd -notin $old -and $_.Path -ieq $target}).Count -gt 0} 15
  Start-Sleep -Milliseconds 1500
  $windows=@(Native-Windows|Where-Object {$_.Hwnd -notin $old -and $_.Path -ieq $target})
  Check ($found -and $windows.Count -eq 1) 'broken bundle registered shell entry returns to exactly one real native target window'
  $windows|Select-Object Hwnd,Path|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $run 'broken-launcher-native.json')
  foreach($window in $windows){[void]$nativeOwned.Add($window.Hwnd)}
 }else{Log '[LIMIT] launcher binary unavailable; broken-bundle fallback unverified'}
 }
 Log '[LIMIT] No real USB or UNC fixture supplied; these device/network routes remain unverified.'
} catch {Log ('[ERROR] '+$_.Exception.Message);$failures++}
finally {
 try {
  if(!$script:process){
   $pidFile=Join-Path $run 'relay-pid.txt'
   if(Test-Path -LiteralPath $pidFile){$script:process=Get-Process -Id ([int][IO.File]::ReadAllText($pidFile)) -ErrorAction SilentlyContinue}
  }
  Stop-OwnedPulse
  foreach($window in @(Native-Windows)){if($nativeOwned.Contains($window.Hwnd) -and $window.Hwnd -notin $nativeBefore.Hwnd){$window.Com.Quit()}}
 } catch {Log ('[CLEANUP-ERROR] '+$_.Exception.Message);$failures++}
 finally {
  try {
   Restore-Registry
   $afterText=Canonical (Snapshot)
   [IO.File]::WriteAllText((Join-Path $run 'registry-after.json'),$afterText)
   Check ($afterText -ceq $beforeText) 'exact HKCU trees, value types and bytes restored'
   Check ((ConvertTo-Json -InputObject @(User-Preferences) -Compress) -ceq $prefsBefore) 'real user preference/session files unchanged'
  }finally{
   foreach($name in $oldEnv.Keys){[Environment]::SetEnvironmentVariable($name,$oldEnv[$name],'Process')}
   $hive.Dispose()
  }
 }
}
Log "Failures: $failures"
Log "Artifacts: $run"
if($failures){exit 1}
