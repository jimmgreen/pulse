param([string]$BuildDir = 'build')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
Push-Location $taskRoot
try {
    $taskBuild = (Resolve-Path -LiteralPath $BuildDir).Path
    $lines = Get-Content (Join-Path $taskBuild 'build.ninja')
    $compile = ($lines | Select-String '^build CMakeFiles.pulse.dir.src.app.address_search.cpp.obj:' | Select-Object -First 1).LineNumber
    $link = ($lines | Select-String '^build pulse.exe:' | Select-Object -First 1).LineNumber
    if (-not $compile -or -not $link) { throw 'Build pulse first with CMake/Ninja.' }
    $compileArgs = ($lines[$compile..($compile + 5)] | Where-Object { $_ -match '^  (DEFINES|FLAGS|INCLUDES) = ' }) -replace '^  \w+ = ', ''
    $compileArgs += "/c src/bench/dialog_jump_focus_test.cpp /Fo`"$taskBuild/dialog_jump_focus.obj`" /Fd`"$taskBuild/dialog_jump_focus_compile.pdb`""
    $compileFile = Join-Path $taskBuild 'dialog_jump_focus_compile.rsp'
    $compileArgs | Set-Content $compileFile -Encoding utf8
    & cl.exe /nologo "@$compileFile"
    if ($LASTEXITCODE) { throw 'Dialog jump focus test compilation failed.' }
    $objects = (($lines[$link - 1] -replace '^build pulse.exe: \S+ ', '') -split ' \|' | Select-Object -First 1)
    $objects = ($objects -split ' ' | Where-Object { $_ -notmatch 'dialog_jump\.cpp\.obj$' }) -join ' '
    $libraries = ($lines[$link..($link + 8)] | Where-Object { $_ -match '^  LINK_LIBRARIES = ' }) -replace '^  LINK_LIBRARIES = ', ''
    @($objects, $libraries, 'dialog_jump_focus.obj /out:pulse_dialog_jump_focus_test.exe /subsystem:console /machine:x64 /INCREMENTAL:NO /MANIFEST:NO /DELAYLOAD:lumatext.dll') |
        Set-Content (Join-Path $taskBuild 'dialog_jump_focus_link.rsp') -Encoding utf8
    Push-Location $taskBuild
    try {
        & link.exe /nologo '@dialog_jump_focus_link.rsp'
        if ($LASTEXITCODE) { throw 'Dialog jump focus test linking failed.' }
        & '.\pulse_dialog_jump_focus_test.exe'
        if ($LASTEXITCODE) { throw 'Dialog jump focus test failed.' }
    } finally { Pop-Location }
} finally { Pop-Location }
