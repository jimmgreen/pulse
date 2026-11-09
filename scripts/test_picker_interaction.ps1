param([string]$BuildDir = 'build')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $build = (Resolve-Path $BuildDir).Path
    $lines = Get-Content (Join-Path $build 'build.ninja')
    $compile = ($lines | Select-String '^build CMakeFiles.pulse_picker_ui_proof.dir.src.ui.folder_picker_dialog.cpp.obj:' | Select-Object -First 1).LineNumber
    $link = ($lines | Select-String '^build PulsePickerUiProof.dll ' | Select-Object -First 1).LineNumber
    $args = ($lines[$compile..($compile+7)] | Where-Object { $_ -match '^  (DEFINES|FLAGS|INCLUDES) = ' }) -replace '^  \w+ = ', ''
    foreach ($source in @('src/ui/folder_picker_dialog.cpp','src/bench/picker_interaction_live_test.cpp')) {
        $name = [IO.Path]::GetFileNameWithoutExtension($source)
        $rsp = Join-Path $build "$name-probe.rsp"
        @($args, "/DPULSE_WITH_TEST_HOOKS=1 /c $source /Fo`"$build/$name-probe.obj`" /Fd`"$build/$name-probe.pdb`"") | Set-Content $rsp -Encoding utf8
        & cl.exe /nologo "@$rsp"
        if ($LASTEXITCODE) { throw "Compilation failed: $source" }
    }
    $objects = (($lines[$link-1] -replace '^build PulsePickerUiProof.dll .*?: \S+ ', '') -split ' \|' | Select-Object -First 1) -replace 'CMakeFiles\\pulse_picker_ui_proof.dir\\src\\ui\\folder_picker_dialog.cpp.obj ', ''
    $objects = $objects -replace 'CMakeFiles\\pulse_picker_ui_proof.dir\\src\\picker_inprocess\\picker_ui.rc.res', 'CMakeFiles\pulse.dir\src\app\pulse.rc.res'
    $libraries = ($lines[$link..($link+8)] | Where-Object { $_ -match '^  LINK_LIBRARIES = ' }) -replace '^  LINK_LIBRARIES = ', ''
    @($objects, $libraries, 'folder_picker_dialog-probe.obj picker_interaction_live_test-probe.obj /out:pulse_picker_interaction_live_test.exe /pdb:picker_interaction_live_test.pdb /DEBUG /subsystem:console /machine:x64 /INCREMENTAL:NO /MANIFEST:NO /DELAYLOAD:lumatext.dll') | Set-Content (Join-Path $build 'picker-interaction-link.rsp') -Encoding utf8
    Push-Location $build
    try {
        & link.exe /nologo '@picker-interaction-link.rsp'
        if ($LASTEXITCODE) { throw 'Picker fixture linking failed' }
    } finally { Pop-Location }
} finally { Pop-Location }
