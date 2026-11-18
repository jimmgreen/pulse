param([string]$BuildDir='build',[switch]$Baseline)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
$build=(Resolve-Path $BuildDir).Path
$lines=Get-Content (Join-Path $build 'build.ninja')
$compile=($lines|Select-String '^build CMakeFiles.pulse.dir.src.app.address_search.cpp.obj:'|Select-Object -First 1).LineNumber
$link=($lines|Select-String '^build pulse.exe:'|Select-Object -First 1).LineNumber
$args=($lines[$compile..($compile+5)]|Where-Object {$_ -match '^  (DEFINES|FLAGS|INCLUDES) = '}) -replace '^  \w+ = ',''
$name=if($Baseline){'wall_baseline'}else{'wall_modified'}
@($args,"/c src/bench/wall_cache_lifetime_test.cpp /Fo`"$build/$name.obj`" /Fd`"$build/$name.pdb`"")|Set-Content (Join-Path $build "$name.rsp") -Encoding utf8
& cl.exe /nologo "@$build/$name.rsp"
if($LASTEXITCODE){throw 'compile failed'}
$objects=(($lines[$link-1] -replace '^build pulse.exe: \S+ ','') -split ' \|'|Select-Object -First 1)
$libraries=($lines[$link..($link+8)]|Where-Object {$_ -match '^  LINK_LIBRARIES = '}) -replace '^  \w+ = ',''
@($objects,$libraries,"$name.obj psapi.lib /out:$name.exe /subsystem:console /machine:x64 /INCREMENTAL:NO /MANIFEST:NO /DELAYLOAD:lumatext.dll")|Set-Content (Join-Path $build "$name-link.rsp") -Encoding utf8
Push-Location $build
try {& link.exe /nologo "@$name-link.rsp";if($LASTEXITCODE){throw 'link failed'}}finally{Pop-Location}
}finally{Pop-Location}
