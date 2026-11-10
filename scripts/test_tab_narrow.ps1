param([string]$BuildDir = 'build')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $build = (Resolve-Path $BuildDir).Path
    $lines = Get-Content (Join-Path $build 'build.ninja')
    $compile = ($lines | Select-String '^build CMakeFiles.pulse.dir.src.app.address_search.cpp.obj:' | Select-Object -First 1).LineNumber
    $link = ($lines | Select-String '^build pulse.exe:' | Select-Object -First 1).LineNumber
    $compileArgs = ($lines[$compile..($compile+5)] | Where-Object { $_ -match '^  (DEFINES|FLAGS|INCLUDES) = ' }) -replace '^  \w+ = ', ''
    foreach ($source in @('src/bench/tab_narrow_live_test.cpp')) {
        $name = [IO.Path]::GetFileNameWithoutExtension($source)
        $rsp = Join-Path $build "$name.rsp"
        @($compileArgs, "/c $source /Fo`"$build/$name.obj`" /Fd`"$build/$name.pdb`"") | Set-Content $rsp -Encoding utf8
        & cl.exe /nologo "@$rsp"
        if ($LASTEXITCODE) { throw "Compilation failed: $source" }
    }
    $objects = (($lines[$link-1] -replace '^build pulse.exe: \S+ ', '') -split ' \|' | Select-Object -First 1)
    $libraries = ($lines[$link..($link+8)] | Where-Object { $_ -match '^  LINK_LIBRARIES = ' }) -replace '^  \w+ = ', ''
    @($objects, $libraries, 'tab_narrow_live_test.obj /out:pulse_tab_narrow_live_test.exe /pdb:tab_narrow_live_test.pdb /subsystem:console /machine:x64 /INCREMENTAL:NO /MANIFEST:NO /DELAYLOAD:lumatext.dll') | Set-Content (Join-Path $build 'tab-narrow-link.rsp') -Encoding utf8
    Push-Location $build
    try { & link.exe /nologo '@tab-narrow-link.rsp'; if ($LASTEXITCODE) { throw 'Fixture link failed' } }
    finally { Pop-Location }
} finally { Pop-Location }

