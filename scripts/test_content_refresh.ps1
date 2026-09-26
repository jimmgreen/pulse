# Run from an x64 Visual Studio developer prompt. Reuse the application objects
# so the test exercises the real timer/navigation path without a second UI build.
param(
    [ValidateSet('content_refresh', 'rename_overlay', 'shared_settings', 'realtime_search', 'content_interaction', 'content_progress_ui', 'address_edit_stability', 'content_lifecycle', 'global_search_settings_ui')][string]$Fixture = 'content_refresh',
    [ValidateSet('all', 'content', 'filename', 'dedup', 'automatic-refresh')][string]$Filter = 'all',
    [string]$BuildDir = 'build',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
Push-Location $taskRoot
try {
    if ($Filter -eq 'automatic-refresh') {
        if ($Fixture -ne 'content_refresh') { throw 'Automatic refresh filter requires content_refresh.' }
    } elseif ($Fixture -ne 'realtime_search' -and $Filter -ne 'all') { throw 'Filter applies only to realtime_search.' }
    $taskBuild = (Resolve-Path -LiteralPath $BuildDir).Path
    if (!$SkipBuild) {
        cmake --build $taskBuild --target pulse pulse_index
        if ($LASTEXITCODE) { throw 'Pulse build failed.' }
    }
    $taskLines = Get-Content (Join-Path $taskBuild 'build.ninja')
    $taskCompile = ($taskLines | Select-String '^build CMakeFiles.pulse.dir.src.app.address_search.cpp.obj:' | Select-Object -First 1).LineNumber
    $taskLink = ($taskLines | Select-String '^build pulse.exe:' | Select-Object -First 1).LineNumber
    if (!$taskCompile -or !$taskLink) { throw 'Expected the single-configuration Ninja build.' }
    $taskCompileArgs = ($taskLines[$taskCompile..($taskCompile + 5)] | Where-Object { $_ -match '^  (DEFINES|FLAGS|INCLUDES) = ' }) -replace '^  \w+ = ', ''
    $taskCompileArgs += "/c src/bench/${Fixture}_test.cpp /Fo`"$taskBuild/${Fixture}_test.obj`" /Fd`"$taskBuild/${Fixture}_test_compile.pdb`""
    $taskCompileResponse = Join-Path $taskBuild 'content_refresh_compile.rsp'
    $taskCompileArgs | Set-Content $taskCompileResponse -Encoding utf8
    & cl.exe /nologo "@$taskCompileResponse"
    if ($LASTEXITCODE) { throw 'Content refresh test compilation failed.' }
    $taskObjects = ($taskLines[$taskLink - 1] -replace '^build pulse.exe: \S+ ', '') -split ' \|' | Select-Object -First 1
    $taskLibraries = ($taskLines[$taskLink..($taskLink + 8)] | Where-Object { $_ -match '^  LINK_LIBRARIES = ' }) -replace '^  LINK_LIBRARIES = ', ''
    $taskDelayLoad = if (($taskLibraries -join ' ') -match '(?i)(?:^|[\\/\s"])lumatext\.lib(?:$|[\s"])') { '/DELAYLOAD:lumatext.dll' } else { '' }
    @($taskObjects, $taskLibraries, "${Fixture}_test.obj /out:pulse_${Fixture}_test.exe /pdb:${Fixture}_test.pdb /subsystem:console /machine:x64 /INCREMENTAL:NO /MANIFEST:NO $taskDelayLoad") | Set-Content (Join-Path $taskBuild 'content_refresh_link.rsp') -Encoding utf8
    Push-Location $taskBuild
    try {
        & link.exe /nologo '@content_refresh_link.rsp'
        if ($LASTEXITCODE) { throw 'Content refresh test linking failed.' }
        if ($Filter -eq 'automatic-refresh') { & ".\pulse_${Fixture}_test.exe" '--automatic-refresh' }
        elseif ($Fixture -eq 'realtime_search') { & ".\pulse_${Fixture}_test.exe" $Filter }
        else { & ".\pulse_${Fixture}_test.exe" }
        if ($LASTEXITCODE) { throw 'Content refresh test failed.' }
    } finally { Pop-Location }
} finally { Pop-Location }
