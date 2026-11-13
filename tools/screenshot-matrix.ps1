<#
.SYNOPSIS
    Renders a fixed set of configurations to PNG screenshots, for before/after comparisons of a code change.

.EXAMPLE
    .\tools\screenshot-matrix.ps1 -OutDir captures\matrix-before
    # ... change code, rebuild ...
    .\tools\screenshot-matrix.ps1 -OutDir captures\matrix-after
    .\tools\compare-images.ps1 captures\matrix-before captures\matrix-after

.NOTES
    Every shot uses --time (frozen animations) and a fixed camera, so the same build renders identical pixels.
    Each line of $Shots is: name, then the arguments for that shot.
#>
param(
    [string] $OutDir = '',
    [string] $Preset = 'x64-release',
    [switch] $GpuValidation, # also run each shot with GPU-based validation (slow) and fail on D3D12 messages
    [string] $Filter = '*'   # only shots whose name matches
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "build\$Preset\bin\WalnutRenderer.exe"
if (-not (Test-Path $exe)) { throw "Build $Preset first: $exe not found" }
if (-not $OutDir) { $OutDir = Join-Path $repo ("captures\matrix_{0:yyyyMMdd_HHmmss}" -f (Get-Date)) }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path # absolute: the app runs in its own bin directory

$start = ''                                     # the scene's default start view (Sponza: down the hall)
$arcade = '--camera=6,1.2,4,-120,-10'           # side arcade: columns, curtains, leaves
$night = '--set=sun.intensity=0 --set=ambient.intensity=0.02'
$lamps = '--set=points.count=256'
$common = '--time=2'

$Shots = @(
    "fwd_lit $start",
    "fwd_noprepass $start --set=view.prepass=0",
    "def_lit $start --set=view.path=deferred",
    "fwd_sunray $start --set=sun.technique=ray",
    "def_sunray $start --set=view.path=deferred --set=sun.technique=ray",
    "fwd_lambert $start --set=lighting.shading=lambert",
    "fwd_brute $arcade $night $lamps --set=points.clustered=0",
    "fwd_cube $arcade $night $lamps --set=points.shadows=cube --set=points.budget=32",
    "fwd_pointray $arcade $night $lamps --set=points.shadows=ray",
    "def_cube $arcade $night $lamps --set=view.path=deferred --set=points.shadows=cube --set=points.budget=32",
    "def_pointray $arcade $night $lamps --set=view.path=deferred --set=points.shadows=ray",
    "fwd_noshadows $arcade $night $lamps --set=points.shadows=off --set=points.markers=0"
)
# Every debug view, forward and (where it exists) deferred.
$views = 'lit','lighting','basecolor','normals','texcoords','instances','materials','alphamode','sunshadow','shadowdiff',
         'roughness','metallic','normalmap','occlusion','emissive','sunspecular','ambient','geonormals','pointlights',
         'clusterlights','pointshadows','pointshadowdiff'
$forwardOnly = 'texcoords','instances','materials','alphamode','normalmap'
foreach ($v in $views) {
    $Shots += "view_fwd_$v $arcade $lamps --set=view.show=$v"
    if ($forwardOnly -notcontains $v) { $Shots += "view_def_$v $arcade $lamps --set=view.path=deferred --set=view.show=$v" }
}

$failed = 0
foreach ($shot in $Shots) {
    $parts = $shot -split ' ', 2
    $name = $parts[0]
    if ($name -notlike $Filter) { continue }
    $argList = @($common) + @($parts[1] -split ' ' | Where-Object { $_ }) + "--screenshot=$(Join-Path $OutDir "$name.png")"
    if ($GpuValidation) { $argList += '--gpu-validation' }
    $p = Start-Process $exe -ArgumentList $argList -WorkingDirectory (Split-Path $exe) -PassThru -Wait -WindowStyle Normal
    $log = Join-Path (Split-Path $exe) 'WalnutRenderer.log'
    $problems = Select-String -Path $log -Pattern 'D3D12 \[|\[error\]|--set: ' | ForEach-Object Line
    if ($p.ExitCode -ne 0 -or $problems) {
        $failed++
        Write-Host "FAIL $name (exit $($p.ExitCode))" -ForegroundColor Red
        $problems | Select-Object -First 5 | ForEach-Object { Write-Host "     $_" }
    } else {
        Write-Verbose "ok   $name"
    }
}
Write-Host "$($Shots.Count) shots -> $OutDir, $failed failed"
if ($failed) { exit 1 }
