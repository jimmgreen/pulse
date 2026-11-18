param([string]$BuildDir = 'build_realtime', [switch]$SelectionIdentity, [switch]$CancelOnly, [switch]$EmbeddedPreview, [switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
Push-Location $taskRoot
try {
    $taskBuild = (Resolve-Path -LiteralPath $BuildDir).Path
    $taskLines = Get-Content (Join-Path $taskBuild 'build.ninja')
    $taskCompile = ($taskLines | Select-String '^build CMakeFiles.pulse.dir.src.app.address_search.cpp.obj:' | Select-Object -First 1).LineNumber
    $taskLink = ($taskLines | Select-String '^build pulse.exe:' | Select-Object -First 1).LineNumber
    $taskCompileArgs = ($taskLines[$taskCompile..($taskCompile + 5)] | Where-Object { $_ -match '^  (DEFINES|FLAGS|INCLUDES) = ' }) -replace '^  \w+ = ', ''
    $taskCompileArgs += "/c src/bench/global_search_window_test.cpp /Fo`"$taskBuild/global_search_window_fixture.obj`" /Fd`"$taskBuild/global_search_window_fixture_compile.pdb`""
    $taskCompileResponse = Join-Path $taskBuild 'global_search_window_fixture_compile.rsp'
    $taskCompileArgs | Set-Content $taskCompileResponse -Encoding utf8
    & cl.exe /nologo "@$taskCompileResponse"
    if ($LASTEXITCODE) { throw 'Window fixture compilation failed.' }
    $taskObjects = (($taskLines[$taskLink - 1] -replace '^build pulse.exe: \S+ ', '') -split ' \|' | Select-Object -First 1) -replace 'CMakeFiles\\pulse.dir\\src\\app\\global_search_window.cpp.obj ', ''
    $taskLibraries = ($taskLines[$taskLink..($taskLink + 8)] | Where-Object { $_ -match '^  LINK_LIBRARIES = ' }) -replace '^  LINK_LIBRARIES = ', ''
    @($taskObjects, $taskLibraries, 'global_search_window_fixture.obj /out:pulse_global_search_window_fixture.exe /pdb:global_search_window_fixture.pdb /DEBUG /subsystem:console /machine:x64 /INCREMENTAL:NO /MANIFEST:NO /DELAYLOAD:lumatext.dll') | Set-Content (Join-Path $taskBuild 'global_search_window_fixture_link.rsp') -Encoding utf8
    Push-Location $taskBuild
    try {
        & link.exe /nologo '@global_search_window_fixture_link.rsp'
        if ($LASTEXITCODE) { throw 'Window fixture linking failed.' }
        if ($BuildOnly) { return }
        if ($EmbeddedPreview) {
            & '.\pulse_global_search_window_fixture.exe' '--embedded-preview' (Join-Path $taskRoot 'bench_data/search-embedded-preview')
        } elseif ($CancelOnly) {
            & '.\pulse_global_search_window_fixture.exe' '--cancel-only'
        } elseif ($SelectionIdentity) {
            & '.\pulse_global_search_window_fixture.exe' '--selection-identity'
        } else {
            & '.\pulse_global_search_window_fixture.exe' (Join-Path $taskRoot 'bench_data/global-search-window')
        }
        if ($LASTEXITCODE) { throw 'Window fixture failed.' }
    } finally { Pop-Location }
} finally { Pop-Location }
