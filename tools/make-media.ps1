<#
.SYNOPSIS
    Renders the README's GIFs and the 30 s showcase video from the timelines in tools/recordings/.

.DESCRIPTION
    Each timeline is played with --record (every frame rendered at an exact time and saved as a PNG), then encoded
    with ffmpeg (winget install Gyan.FFmpeg). Output goes to docs/media/; the frames stay in captures/media-work/.

    tools\make-media.ps1                     # everything (needs ~10 GB free for the video's frames)
    tools\make-media.ps1 -Only sun,night     # just these
#>
param(
    [string] $Preset = 'x64-release',
    [string[]] $Only = @(),
    [string] $WorkDir = '',
    [string] $OutDir = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $repo "build\$Preset\bin\WalnutRenderer.exe"
if (-not (Test-Path $exe)) { throw "Build first: $exe not found" }
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) { throw 'ffmpeg not found (winget install Gyan.FFmpeg)' }
if (-not $WorkDir) { $WorkDir = Join-Path $repo 'captures\media-work' }
if (-not $OutDir) { $OutDir = Join-Path $repo 'docs\media' }
New-Item -ItemType Directory -Force $WorkDir, $OutDir | Out-Null
$WorkDir = (Resolve-Path $WorkDir).Path
$OutDir = (Resolve-Path $OutDir).Path
# drawtext reads fonts by relative path from the working directory (Windows drive letters need awkward escaping).
Copy-Item "$env:WINDIR\Fonts\segoeui.ttf", "$env:WINDIR\Fonts\segoeuib.ttf" $WorkDir -Force

function Record([string] $name, [string] $size) {
    $frames = Join-Path $WorkDir "$name"
    Remove-Item -Recurse -Force $frames -ErrorAction SilentlyContinue
    $script = Join-Path $PSScriptRoot "recordings\$name.txt"
    $process = Start-Process $exe -ArgumentList "--size=$size", "--record=`"$script`"", "--record-dir=`"$frames`"" `
                                  -WorkingDirectory (Split-Path $exe) -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "$name`: WalnutRenderer exited with $($process.ExitCode)" }
    $fps = (Select-String $script -Pattern '^fps\s+(\S+)').Matches[0].Groups[1].Value
    Write-Host "$name`: $((Get-ChildItem $frames).Count) frames"
    return @{ Frames = $frames; Fps = $fps }
}

# A caption box in the bottom-left corner, shown from 'from' to 'to' seconds. The text goes through a file: ffmpeg's
# option parser would split text containing ':' or ','.
$script:captionCount = 0
function Caption([string] $text, [double] $from, [double] $to, [int] $size = 22) {
    $file = "caption$($script:captionCount).txt"
    $script:captionCount++
    [IO.File]::WriteAllText((Join-Path $WorkDir $file), $text, (New-Object Text.UTF8Encoding $false))
    "drawtext=fontfile=segoeui.ttf:textfile=$($file):fontsize=$($size):fontcolor=white:box=1:boxcolor=black@0.55:boxborderw=8:x=14:y=h-th-24:enable='between(t,$from,$to)'"
}

# GIFs are 256 colors per frame. Ordered (Bayer) dithering compresses far better than error diffusion, which matters
# for scenes where everything moves (the lamps).
function Gif([string] $name, [string[]] $captions = @(), [int] $width = 720, [int] $fps = 20, [int] $bayerScale = 3) {
    $r = Record $name '960x540'
    $filters = @("fps=$fps") + $captions + @("scale=$($width):-1:flags=lanczos")
    $chain = ($filters -join ',') + ",split[a][b];[a]palettegen=stats_mode=full[p];[b][p]paletteuse=dither=bayer:bayer_scale=$bayerScale"
    Push-Location $WorkDir
    try {
        & ffmpeg -hide_banner -loglevel error -y -framerate $r.Fps -i "$($r.Frames)\frame_%05d.png" -filter_complex $chain -loop 0 (Join-Path $OutDir "$name.gif")
        if ($LASTEXITCODE -ne 0) { throw "$name`: ffmpeg failed" }
    }
    finally { Pop-Location }
    Write-Host ("{0}.gif: {1:N1} MB" -f $name, ((Get-Item (Join-Path $OutDir "$name.gif")).Length / 1MB))
}

function Wanted([string] $name) { return $Only.Count -eq 0 -or $Only -contains $name }

if (Wanted 'sun') { Gif 'sun' }
if (Wanted 'night') { Gif 'night' -width 640 -fps 15 -bayerScale 4 }
if (Wanted 'clusters') { Gif 'clusters' @((Caption 'Lights per cluster as the lamp count goes 32 → 1024 → 32 (blue: few, red: many)' 0 6 20)) }
if (Wanted 'sunshadows') {
    Gif 'sunshadows' @((Caption 'Sun shadow: 2048² shadow map, 3×3 PCF' 0 1.5), (Caption 'Sun shadow: one ray per pixel (inline ray tracing)' 1.5 3),
                       (Caption 'Difference: red = only the map shadows, blue = only the ray' 3 4.5))
}
if (Wanted 'pointshadows') {
    Gif 'pointshadows' @((Caption 'Point lights without shadows' 0 1.5), (Caption 'Cube shadow maps (6 × 256² depth faces per lamp)' 1.5 3),
                         (Caption 'Ray traced: every lamp casts shadows' 3 4.5))
}

if (Wanted 'showcase') {
    $r = Record 'showcase' '1920x1080'
    # Captions fade in and out; the title card darkens the frame behind it.
    function FadeCaption([string] $text, [double] $a, [double] $b, [double] $fade = 0.35) {
        $alpha = "if(lt(t,$a),0,if(lt(t,$($a + $fade)),(t-$a)/$fade,if(lt(t,$($b - $fade)),1,if(lt(t,$b),($b-t)/$fade,0))))"
        "drawtext=fontfile=segoeui.ttf:text='$text':fontsize=38:fontcolor=white:box=1:boxcolor=black@0.5:boxborderw=16:x=64:y=h-120:alpha='$alpha'"
    }
    $vf = @(
        (FadeCaption 'PBR materials · soft shadow maps' 0.4 5.8),
        (FadeCaption 'The sun moves · its shadow map is re-rendered every frame' 6.2 10.8),
        (FadeCaption '512 animated point lights · clustered shading · cube-map shadows' 11.6 19.8),
        (FadeCaption 'Debug view · base color' 20.0 21.25 0.15),
        (FadeCaption 'Debug view · normals' 21.25 22.5 0.15),
        (FadeCaption 'Debug view · lights per cluster' 22.5 23.75 0.15),
        (FadeCaption 'Debug view · point shadows, cube maps vs rays' 23.75 25.0 0.15),
        "eq=brightness='-0.22*min(1,max(0,(t-25.5)/0.7))':saturation='1-0.3*min(1,max(0,(t-25.5)/0.7))':eval=frame",
        "drawtext=fontfile=segoeuib.ttf:text='WalnutRenderer':fontsize=110:fontcolor=white:shadowcolor=black@0.7:shadowx=3:shadowy=3:x=(w-text_w)/2:y=(h-text_h)/2-50:alpha='if(lt(t,25.8),0,min(1,(t-25.8)/0.6))'",
        "drawtext=fontfile=segoeui.ttf:text='A Direct3D 12 learning renderer · bindless · Enhanced Barriers · inline ray tracing':fontsize=36:fontcolor=white:shadowcolor=black@0.8:shadowx=2:shadowy=2:x=(w-text_w)/2:y=h/2+40:alpha='if(lt(t,26.3),0,min(1,(t-26.3)/0.6))'",
        'fade=t=in:st=0:d=0.5',
        'fade=t=out:st=29.3:d=0.7'
    ) -join ",`n"
    $vfFile = Join-Path $WorkDir 'showcase.vf'
    [IO.File]::WriteAllText($vfFile, $vf, (New-Object Text.UTF8Encoding $false))
    $mp4 = Join-Path $OutDir 'showcase.mp4'
    $small = Join-Path $OutDir 'showcase-720p.mp4'
    Push-Location $WorkDir
    try {
        & ffmpeg -hide_banner -loglevel error -y -framerate $r.Fps -i "$($r.Frames)\frame_%05d.png" -/vf $vfFile `
                 -c:v libx264 -preset slow -crf 20 -pix_fmt yuv420p -movflags +faststart $mp4
        if ($LASTEXITCODE -ne 0) { throw 'showcase: ffmpeg failed' }
        # Under 10 MB, so it can be dropped into GitHub's editor and play inline.
        $scale = 'scale=1280:720:flags=lanczos,fps=30'
        & ffmpeg -hide_banner -loglevel error -y -i $mp4 -vf $scale -c:v libx264 -preset slow -b:v 2400k -maxrate 3000k -bufsize 6000k -pass 1 -an -f null NUL
        & ffmpeg -hide_banner -loglevel error -y -i $mp4 -vf $scale -c:v libx264 -preset slow -b:v 2400k -maxrate 3000k -bufsize 6000k -pass 2 -pix_fmt yuv420p -movflags +faststart $small
        Remove-Item ffmpeg2pass-0.log* -ErrorAction SilentlyContinue
        # A still for the README to link the video from.
        & ffmpeg -hide_banner -loglevel error -y -ss 27.5 -i $mp4 -frames:v 1 -vf 'scale=1280:-1' (Join-Path $OutDir 'showcase-poster.jpg')
    }
    finally { Pop-Location }
    foreach ($f in $mp4, $small) { Write-Host ("{0}: {1:N1} MB" -f (Split-Path $f -Leaf), ((Get-Item $f).Length / 1MB)) }
}
