# Explicit live Windows test. Run only while no user Pulse instance is active.
# Build the small driver in an x64 developer prompt (no full build required):
# cl /std:c++20 /EHsc /utf-8 /W4 /MT /Fe:bench_data\shell_entry_live_driver.exe /Fo:bench_data\ src\bench\shell_entry_live_driver.cpp src\app\shell_integration_registry.cpp advapi32.lib shlwapi.lib shell32.lib user32.lib ole32.lib
# -WinEOnly selects the three real keyboard routes and disabled/native return.
param(
    [string]$Pulse = (Join-Path $PSScriptRoot '..\build\pulse.exe'),
    [string]$Driver = (Join-Path $PSScriptRoot '..\bench_data\shell_entry_live_driver.exe'),
    [switch]$RemainingOnly,
    [switch]$SwitchesOnly,
    [switch]$WinEOnly,
    [switch]$NativeFallbackProbe,
    [switch]$BusyOnly
)
if($NativeFallbackProbe -or $BusyOnly){$WinEOnly=$true}
$ErrorActionPreference = 'Stop'
$Pulse = (Resolve-Path -LiteralPath $Pulse).Path
$Driver = (Resolve-Path -LiteralPath $Driver).Path
$repo = Split-Path $PSScriptRoot -Parent
$run = Join-Path $repo ('bench_data\shell-entry-live-' + [guid]::NewGuid().ToString())
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
function Invoke-Driver([string[]]$Arguments) {
 if($Arguments[0] -eq 'wine') {
  $key=$hive.OpenSubKey('Software\Classes\CLSID\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\shell\opennewwindow\command')
  try {Log ('WIN-E-BEFORE command='+$(if($key){$key.GetValue('')}else{'ABSENT'}))}finally{if($key){$key.Dispose()}}
 }
 $output=& $Driver @Arguments 2>&1
 $output | ForEach-Object { Log ([string]$_) }
 if($Arguments[0] -eq 'apply'){$script:expectedRegistry=Snapshot;$script:expectedRegistry|Export-Clixml -LiteralPath (Join-Path $run 'registry-last-written.clixml') -Depth 30}
 if($LASTEXITCODE -ne 0) { throw "Driver failed: $Arguments" }
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
  $presented=[long](($lines | Where-Object { $_.StartsWith("PRESENTED`t") }).Split("`t")[1])
  [pscustomobject]@{Windows=$windows;Tabs=($windows|Measure-Object Tabs -Sum).Sum;Mode=[int]$mode[1];Compat=[int]$mode[2];Enabled=[int]$mode[3];PcMode=[int]$mode[4];WinEScope=[int]$wine[1];WinERegistered=[int]$wine[2];WinEActive=[int]$wine[3];Presented=$presented}
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
 [IO.File]::WriteAllText((Join-Path $run 'live-control.txt'),"$Mode $Compat $Enabled $PcMode")
 if(!(Wait-For { $s=State; $s -and $s.Mode -eq $Mode -and $s.Compat -eq $Compat -and $s.Enabled -eq $Enabled -and $s.PcMode -eq $PcMode } 10)) {throw 'Live mode not acknowledged by a real presented frame'}
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
try {
 foreach($name in @('baseline','folder-entry','compat-on','compat-off','all-off')) {[void][IO.Directory]::CreateDirectory((Join-Path $run $name))}
 $preferences=@{startup_open=1;home_folder=(Join-Path $run 'baseline');new_window_shows=1;external_open=1;open_this_pc=1;open_win_e=1;keep_running_on_close=$false;integration_enabled=$false;integration_folders=$false;integration_win_e=$true;integration_this_pc=$false;integration_recycle_bin=$false;integration_folders_compat=$false;take_over_explorer_windows=$false;auto_check_updates=$false}
 [IO.File]::WriteAllText((Join-Path $run 'app.json'),($preferences|ConvertTo-Json))
 [IO.File]::WriteAllText((Join-Path $run 'live-control.txt'),'1 0 0')
 $env:PULSE_TEST_DATA_DIR=$run;$env:PULSE_TEST_LIVE_SHELL_ENTRY='1';$env:PULSE_TEST_WIN_E_HOTKEY='1'
 # This is the real visible application under test, not a background helper.
 $process=Start-Process -FilePath $Pulse -ArgumentList ('"'+(Join-Path $run 'baseline')+'"') -PassThru -WindowStyle Normal
 if(!(Wait-For { Presented-Path (Join-Path $run 'baseline') })){throw 'Owned real Pulse primary did not present its fixture baseline'}
 Log "PRIMARY pid=$($process.Id) profile=$run"
 $script:expectedRegistry=Snapshot
 Set-Mode 1 0 1
 if(!$WinEOnly) {
 Invoke-Driver -Arguments @('apply','folders','on',$Pulse)
 if(!$RemainingOnly -and !$SwitchesOnly) {
 Open-PulsePath (Join-Path $run 'folder-entry') (Join-Path $run 'folder-entry') 'folder'
 Open-PulsePath ($env:SystemDrive+'\') ($env:SystemDrive+'\') 'drive'
 Invoke-Driver -Arguments @('apply','pc','on',$Pulse)
 Open-PulsePath 'shell:MyComputerFolder' '' 'this-pc'
 foreach($pcMode in @(0,1,2)) {
  Set-Mode 1 0 1 $pcMode
  $start=State
  if($pcMode -eq 2){foreach($window in $start.Windows){[void][ShellEntryWindows]::ShowWindow([IntPtr]$window.Hwnd,6)}}
  Invoke-Driver -Arguments @('shell','shell:MyComputerFolder')
  $ok=Wait-For {
   $s=State;if(!$s){return $false}
   if($pcMode -eq 0){return $s.Windows.Count -eq $start.Windows.Count+1 -and $s.Tabs -eq $start.Tabs+1 -and (Presented-Path '')}
   if($pcMode -eq 1){return $s.Windows.Count -eq $start.Windows.Count -and $s.Tabs -eq $start.Tabs+1 -and (Presented-Path '')}
   return $s.Windows.Count -eq $start.Windows.Count -and $s.Tabs -eq $start.Tabs -and ([ShellEntryWindows]::GetForegroundWindow().ToInt64() -in $s.Windows.Hwnd)
  }
  Check $ok "This PC mode=${pcMode}: actual ShellExecute produces required window/tab/activation outcome"
  Copy-Item -LiteralPath (Join-Path $run 'live-state.tsv') -Destination (Join-Path $run "this-pc-$pcMode.tsv")
 }
 } else {Log '[SKIP] Previously verified folder/drive/This PC cases excluded by RemainingOnly';Invoke-Driver -Arguments @('apply','pc','on',$Pulse)}
 Invoke-Driver -Arguments @('apply','recycle','on',$Pulse)
 if(!$RemainingOnly -and !$SwitchesOnly){Open-PulsePath 'shell:RecycleBinFolder' 'pulse:recycle' 'recycle-bin'}
 }
 Invoke-Driver -Arguments @('apply','wine','on',$Pulse)
 if(!(Wait-For {$s=State;$s -and $s.WinEScope -eq 1 -and $s.WinERegistered -eq 1 -and $s.WinEActive -eq 1})){throw 'Production registration and active runtime Win+E listener were not both confirmed'}
 $merged=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::ClassesRoot,[Microsoft.Win32.RegistryView]::Registry64)
 try {
  foreach($suffix in @('shell','shell\open\command','shell\opennewwindow\command')) {
   $key=$merged.OpenSubKey('CLSID\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\'+$suffix)
   try {if($key){foreach($name in $key.GetValueNames()){Log "HKCR-WINE $suffix / $name = $($key.GetValue($name)) [type=$($key.GetValueKind($name))]"}}else{Log "HKCR-WINE $suffix ABSENT"}}finally{if($key){$key.Dispose()}}
  }
 }finally{$merged.Dispose()}
 if($NativeFallbackProbe) {
  $file=Join-Path (Join-Path $run 'baseline') 'native-fallback.txt'
  [IO.File]::WriteAllText($file,'native fallback fixture')
  $candidates=@(
   [pscustomobject]@{Command='/n,/e,::{20D04FE0-3AEA-1069-A2D8-08002B30309D}';Expected='::{20D04FE0-3AEA-1069-A2D8-08002B30309D}'},
   [pscustomobject]@{Command='/n,::{20D04FE0-3AEA-1069-A2D8-08002B30309D}';Expected='::{20D04FE0-3AEA-1069-A2D8-08002B30309D}'},
   [pscustomobject]@{Command=('/n,/select,"'+$file+'"');Expected=(Join-Path $run 'baseline')})
  foreach($candidate in $candidates) {
   $old=@(Native-Windows).Hwnd;$pulseBefore=State
   Invoke-Driver -Arguments @('native',$candidate.Command)
   $native=Wait-For {$script:nativeCandidate=@(Native-Windows|Where-Object {$_.Hwnd -notin $old -and $_.Path -ieq $candidate.Expected})|Select-Object -First 1;$null -ne $script:nativeCandidate}
   $pulseAfter=State
   $exclusive=$native -and $pulseAfter.Tabs -eq $pulseBefore.Tabs -and $pulseAfter.Windows.Count -eq $pulseBefore.Windows.Count
   Check $exclusive ('explicit native launch, registered Win+E remains on: '+$candidate.Command)
   if($script:nativeCandidate){[void]$nativeOwned.Add($script:nativeCandidate.Hwnd);Log ('NATIVE-OBSERVED hwnd='+$script:nativeCandidate.Hwnd+' path='+$script:nativeCandidate.Path+' foreground='+[ShellEntryWindows]::GetForegroundWindow().ToInt64())}
   if($exclusive){break}
  }
 }
 if(!$SwitchesOnly -and !$NativeFallbackProbe -and !$BusyOnly) {
 foreach($mode in @(0,1,2)) {
  Set-Mode $mode
  $start=State
  $keyboardNativeBefore=@(Native-Windows).Hwnd
  if($mode -eq 2) {foreach($window in $start.Windows){[void][ShellEntryWindows]::ShowWindow([IntPtr]$window.Hwnd,6)}}
  Invoke-Driver -Arguments @('wine')
  $ok=Wait-For {
   $s=State
   if(!$s){return $false}
   if($mode -eq 0){return $s.Windows.Count -eq $start.Windows.Count+1 -and $s.Tabs -eq $start.Tabs+1 -and (Presented-Path '')}
   if($mode -eq 1){return $s.Windows.Count -eq $start.Windows.Count -and $s.Tabs -eq $start.Tabs+1 -and (Presented-Path '')}
   return $s.Windows.Count -eq $start.Windows.Count -and $s.Tabs -eq $start.Tabs -and ([ShellEntryWindows]::GetForegroundWindow().ToInt64() -in $s.Windows.Hwnd)
  }
  Check $ok "Win+E mode=${mode}: actual SendInput produces required window/tab/activation outcome"
  $duplicateNative=Wait-For {@(Native-Windows|Where-Object {$_.Hwnd -notin $keyboardNativeBefore}).Count -gt 0} 2
  Check (!$duplicateNative) "Win+E mode=${mode}: no duplicate native window during post-delivery observation"
  $delivered=State
  Check ([ShellEntryWindows]::GetForegroundWindow().ToInt64() -in $delivered.Windows.Hwnd) "Win+E mode=${mode}: Pulse owns the final foreground window"
  $keysReleased=([ShellEntryWindows]::GetAsyncKeyState(0x5B) -band 0x8000) -eq 0 -and ([ShellEntryWindows]::GetAsyncKeyState(0x5C) -band 0x8000) -eq 0 -and ([ShellEntryWindows]::GetAsyncKeyState(0x45) -band 0x8000) -eq 0
  Check $keysReleased "Win+E mode=${mode}: Win and E keys are released after delivery"
  @(Native-Windows|Select-Object Hwnd,Path)|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $run "win-e-$mode-native.json")
  foreach($window in @(Native-Windows)){if($window.Hwnd -notin $keyboardNativeBefore -and $window.Path -in @('::{20D04FE0-3AEA-1069-A2D8-08002B30309D}','::{679f85cb-0220-4080-b29b-5540cc05aab6}','::{F874310E-B6B7-47DC-BC84-B9E6B38F5903}')){[void]$nativeOwned.Add($window.Hwnd)}}
  $key=$hive.OpenSubKey('Software\Classes\CLSID\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\shell\opennewwindow\command')
  try {Log ('WIN-E-AFTER command='+$(if($key){$key.GetValue('')}else{'ABSENT'}))}finally{if($key){$key.Dispose()}}
  Copy-Item -LiteralPath (Join-Path $run 'live-state.tsv') -Destination (Join-Path $run "win-e-$mode.tsv")
 }
 }
 if(!$WinEOnly) {
 Invoke-Driver -Arguments @('apply','folders','off',$Pulse)
 Set-Mode 1 1 1
 Open-PulsePath (Join-Path $run 'compat-on') (Join-Path $run 'compat-on') 'compat-on'
 Check (@(Native-Windows | Where-Object {$_.Path -ieq (Join-Path $run 'compat-on')}).Count -eq 0) 'compat-on: native source absent after Pulse presentation'
 Set-Mode 1 0 1
 Open-Native (Join-Path $run 'compat-off') (Join-Path $run 'compat-off') 'compat-off'
 Set-Mode 1 0 0
 foreach($kind in @('folders','wine','pc','recycle')) {Invoke-Driver -Arguments @('apply',$kind,'off',$Pulse)}
 Open-Native (Join-Path $run 'all-off') (Join-Path $run 'all-off') 'total-off-folder'
 Open-Native 'shell:MyComputerFolder' '::{20D04FE0-3AEA-1069-A2D8-08002B30309D}' 'total-off-this-pc'
 Open-Native 'shell:RecycleBinFolder' '::{645FF040-5081-101B-9F08-00AA002F954E}' 'total-off-recycle'
 $oldNative=@(Native-Windows).Hwnd
 $beforeWinE=State
 Invoke-Driver -Arguments @('wine')
 $nativeWinETargets=@('::{20D04FE0-3AEA-1069-A2D8-08002B30309D}','::{679f85cb-0220-4080-b29b-5540cc05aab6}','::{f874310e-b6b7-47dc-bc84-b9e6b38f5903}')
 $offWinE=Wait-For { $script:offWinEWindow=@(Native-Windows | Where-Object {$_.Hwnd -notin $oldNative -and $_.Path -in $nativeWinETargets}) | Select-Object -First 1; $null -ne $script:offWinEWindow }
 Check $offWinE 'total-off-Win+E: actual keyboard shortcut opens native Explorer'
 if($script:offWinEWindow){[void]$nativeOwned.Add($script:offWinEWindow.Hwnd);$script:offWinEWindow.Com.Quit()}
 $afterWinE=State
 Check ($afterWinE.Tabs -eq $beforeWinE.Tabs -and $afterWinE.Windows.Count -eq $beforeWinE.Windows.Count) 'total-off-Win+E: no extra Pulse page/window'
 } elseif(!$NativeFallbackProbe) {
  Set-Mode 1 0 1
  $busyBefore=State
  $busyNativeBefore=@(Native-Windows).Hwnd
  $disabledOwned=@([ShellEntryWindows]::Pulse([uint32]$process.Id))
  try {
   foreach($hwnd in $disabledOwned){[void][ShellEntryWindows]::EnableWindow([IntPtr]$hwnd,$false)}
   Check (@($disabledOwned|Where-Object {[ShellEntryWindows]::IsWindowEnabled([IntPtr]$_)}).Count -eq 0) 'busy Win+E: every owned Pulse window is genuinely disabled'
   $busyStart=[DateTime]::UtcNow
   Invoke-Driver -Arguments @('wine')
   $earlyNative=Wait-For {@(Native-Windows|Where-Object {$_.Hwnd -notin $busyNativeBefore}).Count -gt 0} 2
   Check (!$earlyNative) 'busy Win+E: accepted request does not immediately open native Explorer'
   $nativeTargets=@('::{20D04FE0-3AEA-1069-A2D8-08002B30309D}')
   $fellBack=Wait-For {$script:busyFallback=@(Native-Windows|Where-Object {$_.Hwnd -notin $busyNativeBefore -and $_.Path -in $nativeTargets});$script:busyFallback.Count -gt 0} 40
   $elapsed=([DateTime]::UtcNow-$busyStart).TotalSeconds
   Log ('BUSY-DEADLINE elapsed_seconds='+$elapsed.ToString('F2',[Globalization.CultureInfo]::InvariantCulture))
   foreach($window in $script:busyFallback){Log ('BUSY-NATIVE hwnd='+$window.Hwnd+' path='+$window.Path+' foreground='+[ShellEntryWindows]::GetForegroundWindow().ToInt64())}
   Check ($fellBack -and $elapsed -ge 28) 'busy Win+E: real deferred request deadline returns to native Explorer'
   $duplicates=Wait-For {@(Native-Windows|Where-Object {$_.Hwnd -notin $busyNativeBefore}).Count -gt 1} 2
   $busyAfter=State
   Check ($fellBack -and !$duplicates -and $busyAfter.Tabs -eq $busyBefore.Tabs -and $busyAfter.Windows.Count -eq $busyBefore.Windows.Count) 'busy Win+E failure: exactly one native window and no new Pulse page/window'
   $correlated=Wait-For {
    $events=@(WinE-Events)
    $accepted=@($events|Where-Object {$_.event -eq 'win_e_chord' -and $_.data.accepted -eq 1 -and [DateTime]::Parse($_.utc).ToUniversalTime() -ge $busyStart})|Select-Object -Last 1
    if(!$accepted){return $false}
    return @($events|Where-Object {$_.event -eq 'win_e_delivery' -and $_.data.state -eq 3 -and $_.data.native_fallback -eq 1 -and $_.data.error -eq 0 -and $_.data.id_high -eq $accepted.data.id_high -and $_.data.id_low -eq $accepted.data.id_low}).Count -eq 1
   } 5
   Check $correlated 'busy Win+E trace: accepted request ID ends Cancelled with exactly one successful native fallback'
   @(WinE-Events)|ForEach-Object {$_|ConvertTo-Json -Depth 8 -Compress}|Set-Content -LiteralPath (Join-Path $run 'win-e-runtime.jsonl')
   foreach($window in @(Native-Windows|Where-Object {$_.Hwnd -notin $busyNativeBefore -and $_.Path -in $nativeTargets})){[void]$nativeOwned.Add($window.Hwnd);$window.Com.Quit()}
  }finally{foreach($hwnd in $disabledOwned){[void][ShellEntryWindows]::EnableWindow([IntPtr]$hwnd,$true)};$disabledOwned=@()}
  Set-Mode 1 0 0
  Invoke-Driver -Arguments @('apply','wine','off',$Pulse)
  Check (Wait-For {$s=State;$s -and $s.WinERegistered -eq 0 -and $s.WinEActive -eq 0}) 'Win+E disabled: production registry read and runtime listener confirm restoration'
  if(!$BusyOnly) {
  $oldNative=@(Native-Windows).Hwnd
  $beforeWinE=State
  Invoke-Driver -Arguments @('wine')
  $nativeWinETargets=@('::{20D04FE0-3AEA-1069-A2D8-08002B30309D}','::{679f85cb-0220-4080-b29b-5540cc05aab6}','::{f874310e-b6b7-47dc-bc84-b9e6b38f5903}')
  $offWinE=Wait-For {$script:offWinEWindow=@(Native-Windows|Where-Object {$_.Hwnd -notin $oldNative -and $_.Path -in $nativeWinETargets})|Select-Object -First 1;$null -ne $script:offWinEWindow}
  Check $offWinE 'Win+E disabled: actual keyboard shortcut returns to native Explorer'
  if($script:offWinEWindow){[void]$nativeOwned.Add($script:offWinEWindow.Hwnd);$script:offWinEWindow.Com.Quit()}
  $afterWinE=State
  Check ($afterWinE.Tabs -eq $beforeWinE.Tabs -and $afterWinE.Windows.Count -eq $beforeWinE.Windows.Count) 'Win+E disabled: no extra Pulse page/window'
  }
 }
} catch {Log ('[ERROR] '+$_.Exception.Message);$failures++}
finally {
 try {
  foreach($hwnd in $disabledOwned){[void][ShellEntryWindows]::EnableWindow([IntPtr]$hwnd,$true)}
  if($process) {
   $owned=@([ShellEntryWindows]::Pulse([uint32]$process.Id))
   if($owned.Count){[void][ShellEntryWindows]::PostMessage([IntPtr]$owned[0],0x8048,[IntPtr]::Zero,[IntPtr]::Zero)}
   if(!$process.WaitForExit(15000)){
    $process.Refresh()
    if(!$process.HasExited){try{$process.Kill();$process.WaitForExit();Log '[CLEANUP] Owned test process required termination'}catch{$process.Refresh();if(!$process.HasExited){Log ('[CLEANUP-ERROR] '+$_.Exception.Message);$failures++}}}
   }
  }
  foreach($window in @(Native-Windows)) {if($nativeOwned.Contains($window.Hwnd) -and $window.Hwnd -notin $nativeBefore.Hwnd){$window.Com.Quit()}}
 } catch {Log ('[CLEANUP-ERROR] '+$_.Exception.Message);$failures++}
 finally {
  try {
   Restore-Registry
   $after=Snapshot;$afterText=Canonical $after
   [IO.File]::WriteAllText((Join-Path $run 'registry-after.json'),$afterText)
   Check ($afterText -ceq $beforeText) 'exact before/after HKCU trees, value types and bytes restored'
   Check ((ConvertTo-Json -InputObject @(User-Preferences) -Compress) -ceq $prefsBefore) 'real user preference/session files unchanged'
  } finally {
   foreach($name in $oldEnv.Keys){[Environment]::SetEnvironmentVariable($name,$oldEnv[$name],'Process')}
   $hive.Dispose()
  }
 }
}
Log "Failures: $failures"
Log "Artifacts: $run"
if($failures){exit 1}
