# Creates and cleans only a unique temporary ISO; requires the pulse_drive_eject_test target.
param([string]$ReadyFile = '')
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
public static class PulseIsoStream {
 public static void Save(object source, string path) {
  IStream stream = (IStream)source;
  IntPtr count = Marshal.AllocHGlobal(4);
  try { using(FileStream file = new FileStream(path, FileMode.CreateNew)) {
   byte[] data = new byte[65536];
   while(true) { stream.Read(data, data.Length, count); int size=Marshal.ReadInt32(count); if(size==0) break; file.Write(data,0,size); }
  }} finally { Marshal.FreeHGlobal(count); }
 }
}
"@
$nonce = 'pulseiso' + [Guid]::NewGuid().ToString('N')
$fixture = Join-Path $env:TEMP $nonce
$isoPath = $fixture + '.iso'
New-Item -ItemType Directory -Path $fixture | Out-Null
Set-Content -LiteralPath (Join-Path $fixture ($nonce + '.txt')) -Value $nonce
try {
 $creator = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
 $creator.FileSystemsToCreate = 3
 $creator.VolumeName = 'PULSEISOTEST'
 $creator.Root.AddTree($fixture, $false)
 $result = $creator.CreateResultImage()
 [PulseIsoStream]::Save($result.ImageStream, $isoPath)
 $mounted = Mount-DiskImage -ImagePath $isoPath -PassThru
 $letter = ($mounted | Get-Volume).DriveLetter
 if (!$letter) { throw 'Fixture has no drive letter' }
 if ($ReadyFile) {
  @{ root = ($letter + ':\'); image = $isoPath; marker = $nonce; directory = $fixture } | ConvertTo-Json | Set-Content -LiteralPath $ReadyFile
  $deadline = (Get-Date).AddMinutes(60)
  while ((Get-Date) -lt $deadline -and !(Test-Path -LiteralPath ($ReadyFile + '.done'))) { Start-Sleep -Milliseconds 200 }
  return
 }
 $harness = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\build')).Path ('drive-eject-iso-auth-' + $nonce)
 New-Item -ItemType Directory -Path $harness | Out-Null
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\build\pulse_drive_eject_test.exe') -Destination (Join-Path $harness 'pulse.exe')
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\build\pulse_elevated.exe') -Destination (Join-Path $harness 'pulse_elevated.exe')
 & (Join-Path $harness 'pulse.exe') '--iso' ($letter + ':\') $nonce
 $fixtureTestExit = $LASTEXITCODE
 Write-Output ('TestExit=' + $fixtureTestExit)
 if ($fixtureTestExit -ne 0) { throw 'ISO eject regression failed' }
 if ((Get-DiskImage -ImagePath $isoPath).Attached) { throw 'ISO remains attached after successful eject' }
 Write-Output 'ISO Attached=False'
} finally {
 if (Test-Path -LiteralPath $isoPath) {
  Dismount-DiskImage -ImagePath $isoPath -ErrorAction SilentlyContinue | Out-Null
  Remove-Item -LiteralPath $isoPath -Force
 }
 Remove-Item -LiteralPath (Join-Path $fixture ($nonce + '.txt')) -Force
 Remove-Item -LiteralPath $fixture -Force
}
