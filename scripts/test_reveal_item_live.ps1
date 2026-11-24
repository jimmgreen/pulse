param(
    [string]$Pulse = (Join-Path $PSScriptRoot '..\build\pulse.exe'),
    [switch]$Legacy
)
$ErrorActionPreference = 'Stop'
$Pulse = (Resolve-Path -LiteralPath $Pulse).Path
$repo = Split-Path $PSScriptRoot -Parent
if ((Split-Path (Split-Path $Pulse -Parent) -Parent) -ne $repo -or
    (Split-Path $Pulse -Leaf) -ne 'pulse.exe') { throw 'Use a workspace build.' }

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class RevealItemWindows {
    [StructLayout(LayoutKind.Sequential)]
    struct Msg { public IntPtr hwnd; public uint message; public UIntPtr wp; public IntPtr lp; public uint time; public int x, y; public uint reserved; }
    delegate void Event(IntPtr hook, uint type, IntPtr hwnd, int obj, int child, uint tid, uint time);
    [DllImport("user32")] static extern IntPtr SetWinEventHook(uint min, uint max, IntPtr dll, Event callback, uint pid, uint tid, uint flags);
    [DllImport("user32")] static extern bool UnhookWinEvent(IntPtr hook);
    [DllImport("user32")] static extern bool PeekMessageW(out Msg msg, IntPtr hwnd, uint a, uint b, uint remove);
    [DllImport("user32")] static extern IntPtr DispatchMessageW(ref Msg msg);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder name, int length);
    [DllImport("user32")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    static readonly Event Callback = OnShow;
    static IntPtr hook;
    public static int ExplorerShows, PulseShows;
    static void OnShow(IntPtr h, uint type, IntPtr hwnd, int obj, int child, uint tid, uint time) {
        if (obj != 0 || child != 0 || hwnd == IntPtr.Zero) return;
        var name = new StringBuilder(128);
        GetClassName(hwnd, name, name.Capacity);
        if (name.ToString() == "CabinetWClass" || name.ToString() == "ExploreWClass") ++ExplorerShows;
        if (name.ToString() == "PulseMainWindow") ++PulseShows;
    }
    public static bool Start() {
        hook = SetWinEventHook(0x8002, 0x8002, IntPtr.Zero, Callback, 0, 0, 0);
        return hook != IntPtr.Zero;
    }
    public static void Pump() {
        Msg msg;
        while (PeekMessageW(out msg, IntPtr.Zero, 0, 0, 1)) DispatchMessageW(ref msg);
    }
    public static bool Owned(IntPtr hwnd, uint pid) {
        uint current;
        return GetWindowThreadProcessId(hwnd, out current) != 0 && current == pid;
    }
    public static void Stop() { if (hook != IntPtr.Zero) UnhookWinEvent(hook); }
}
'@

$profile = Join-Path $repo ('bench_data\shell-entry-live-reveal-' + [guid]::NewGuid().ToString('B'))
$folder = Join-Path $profile 'parent'
$target = Join-Path $folder 'child (folder) 中文'
$packageName = 'fixture only 中文.pulseext'
$package = Join-Path $folder $packageName
[void][IO.Directory]::CreateDirectory($target)
[IO.File]::WriteAllText($package, 'Not an installable package. The reveal command must only select this fixture.')
$prefs = @{
    startup_open = 1; home_folder = $profile; external_open = 1; keep_running_on_close = $false
    integration_enabled = $false; take_over_explorer_windows = $false; auto_check_updates = $false
}
[IO.File]::WriteAllText((Join-Path $profile 'app.json'), ($prefs | ConvertTo-Json))
$stateFile = Join-Path $profile 'live-state.tsv'
$old = @{}
foreach ($key in @('PULSE_TEST_DATA_DIR', 'PULSE_TEST_LIVE_SHELL_ENTRY', 'PULSE_STARTUP_TRACE', 'TEMP', 'TMP')) {
    $old[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
}
$owned = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
function Launch([string]$Arguments) {
    Write-Host "COMMAND: `"$Pulse`" $Arguments"
    $p = Start-Process -FilePath $Pulse -ArgumentList $Arguments -WorkingDirectory $profile -PassThru
    Write-Host "PID: $($p.Id)"
    $owned.Add($p)
    return $p
}
function Wait-Result([scriptblock]$Check) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        [RevealItemWindows]::Pump()
        $result = & $Check
        if ($result) { return $result }
        Start-Sleep -Milliseconds 20
    } while ($timer.Elapsed.TotalSeconds -lt 18)
    throw "Timed out; fixture: $profile"
}
function Presented([string]$Directory, [string]$Item, [uint32]$PidToCheck) {
    if ($primary.Id -eq $PidToCheck -and $primary.HasExited) {
        throw "Primary exited before presentation: pid=$PidToCheck status=$($primary.ExitCode)"
    }
    if (!(Test-Path -LiteralPath $stateFile)) { return }
    $rows = [IO.File]::ReadAllLines($stateFile, [Text.Encoding]::UTF8)
    if ($rows -notcontains "PID`t$PidToCheck") { return }
    foreach ($row in $rows) {
        $fields = $row -split "`t"
        if ($fields.Length -ne 6 -or $fields[0] -ne 'WINDOW' -or $fields[3] -ne '0' -or
            $fields[4] -eq '0') { continue }
        $current = $fields[5]
        if ($current.StartsWith('\\?\')) { $current = $current.Substring(4) }
        if ($current -ne $Directory) { continue }
        $hwnd = [IntPtr][long]$fields[1]
        if (![RevealItemWindows]::Owned($hwnd, $PidToCheck) -or ![RevealItemWindows]::IsWindowVisible($hwnd)) { continue }
        $selected = @($rows | Where-Object { $_.StartsWith("SELECTED`t$($fields[1])`t") })
        if ($Item -and ($selected.Count -ne 1 -or $selected[0] -cne "SELECTED`t$($fields[1])`t$Item")) { continue }
        if (!$Item -and $selected.Count -ne 0) { continue }
        return $hwnd
    }
}
function Check-Exit($Process, [int]$Expected) {
    [void](Wait-Result { $Process.HasExited })
    if ($Process.ExitCode -ne $Expected) { throw "pid=$($Process.Id) exit=$($Process.ExitCode) expected=$Expected" }
    Write-Host "EXIT: pid=$($Process.Id) status=$($Process.ExitCode)"
}

try {
    $env:PULSE_TEST_DATA_DIR = $profile
    $env:PULSE_TEST_LIVE_SHELL_ENTRY = '1'
    $env:PULSE_STARTUP_TRACE = '1'
    $env:TEMP = $profile
    $env:TMP = $profile
    if (![RevealItemWindows]::Start()) { throw 'Cannot observe window-show events.' }
    $coldArgs = if ($Legacy) { "`"$target`"" } else { "--reveal-item `"$target`"" }
    $primary = Launch $coldArgs
    $hwnd = if ($Legacy) {
        Wait-Result { Presented $target '' $primary.Id }
    } else {
        Wait-Result { Presented $folder (Split-Path $target -Leaf) $primary.Id }
    }
    if ($Legacy) {
        Write-Host "[BASELINE] folder opened; no item selected: $target"
    } else {
        Write-Host "[PASS] cold: folder=$folder selected=$(Split-Path $target -Leaf)"
        $forwarded = Launch "--reveal-item `"$package`""
        Check-Exit $forwarded 0
        $same = Wait-Result { Presented $folder $packageName $primary.Id }
        if ($same -ne $hwnd) { throw 'Warm reveal changed the target window.' }
        Write-Host "[PASS] warm: same primary and window, selected=$packageName"
        $missing = Join-Path $folder 'missing reveal item.txt'
        if (Test-Path -LiteralPath $missing) { throw "Unexpected fixture: $missing" }
        Check-Exit (Launch "--reveal-item `"$missing`"") 2
        [void](Wait-Result { Presented $folder $packageName $primary.Id })
        Write-Host '[PASS] missing target: file-not-found; current selection unchanged'
        $invalid = Launch '--reveal-item --win-e'
        Check-Exit $invalid 87
        [void](Wait-Result { Presented $folder $packageName $primary.Id })
        Write-Host '[PASS] invalid target: current selection unchanged'
    }
    [void][RevealItemWindows]::PostMessageW($hwnd, 0x8048, [IntPtr]::Zero, [IntPtr]::Zero)
    Check-Exit $primary 0
    [RevealItemWindows]::Pump()
    if ([RevealItemWindows]::PulseShows -lt 1 -or [RevealItemWindows]::ExplorerShows -ne 0) {
        throw "show events: Pulse=$([RevealItemWindows]::PulseShows) Explorer=$([RevealItemWindows]::ExplorerShows)"
    }
    Write-Host "[PASS] show events: Pulse=$([RevealItemWindows]::PulseShows) Explorer=$([RevealItemWindows]::ExplorerShows)"
} finally {
    [RevealItemWindows]::Stop()
    foreach ($p in $owned) {
        if (!$p.HasExited) { $p.Kill(); [void]$p.WaitForExit(5000) }
        $p.Dispose()
    }
    foreach ($key in $old.Keys) { [Environment]::SetEnvironmentVariable($key, $old[$key], 'Process') }
}
