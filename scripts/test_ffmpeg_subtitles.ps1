param([Parameter(Mandatory = $true)][string]$FFmpeg)
$ErrorActionPreference = 'Stop'
$ff = (Resolve-Path $FFmpeg).Path
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('pulse-subtitles-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temporary | Out-Null
Push-Location $temporary
try {
    # The subtitle is active only after seeking. Resetting timestamps without
    # restoring the source offset would produce a black frame and fail this test.
    [IO.File]::WriteAllBytes((Join-Path $temporary 'frames.rgb'), [byte[]]::new(320 * 180 * 3 * 4))
    [IO.File]::WriteAllText((Join-Path $temporary 'sample.srt'), "1`n00:00:01,000 --> 00:00:02,000`nPulse subtitle smoke`n", [Text.UTF8Encoding]::new($false))
    & $ff -v error -y -f rawvideo -pixel_format rgb24 -video_size 320x180 -framerate 2 -i frames.rgb -ss 1 -frames:v 1 -vf 'setpts=PTS-STARTPTS+1/TB,subtitles=sample.srt' -pix_fmt rgb24 -f rawvideo rendered.rgb
    if ($LASTEXITCODE -ne 0) { throw 'Subtitle rendering command failed' }
    $pixels = [IO.File]::ReadAllBytes((Join-Path $temporary 'rendered.rgb'))
    if ($pixels.Length -ne 320 * 180 * 3) { throw 'Subtitle output frame has invalid size' }
    if (-not ($pixels | Where-Object { $_ -gt 32 } | Select-Object -First 1)) { throw 'Subtitle did not render any visible pixels' }
    Write-Host '[PASS] Subtitle text rendered with source timestamp offset'
} finally {
    Pop-Location
    $resolved = (Resolve-Path -LiteralPath $temporary).Path
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notmatch '^pulse-subtitles-[a-f0-9]{32}$') {
        throw 'Refusing cleanup outside the isolated subtitle fixture directory'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
