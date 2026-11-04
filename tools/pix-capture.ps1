<#
.SYNOPSIS
    Takes a PIX Timing Capture or GPU Capture of WalnutRenderer using pixtool.

.EXAMPLE
    .\tools\pix-capture.ps1 -Kind Timing -DurationMs 3000
    .\tools\pix-capture.ps1 -Kind Gpu -Frames 1 -Preset x64-debug

.NOTES
    Captures go to <repo>\captures (git-ignored). Timing Captures can then be analyzed with the
    PixMcp MCP server (.vscode/mcp.json) or opened in the PIX UI.
#>
param(
    [ValidateSet('Timing', 'Gpu')] [string] $Kind = 'Timing',
    [string] $Preset = 'x64-release',
    [int] $DurationMs = 3000,     # Timing only
    [int] $Frames = 1,            # GPU only
    [string] $AppArgs = '',
    [string] $Output = ''
)
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$pixDir = Get-ChildItem "$env:ProgramFiles\Microsoft PIX" -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName 'pixtool.exe') } |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $pixDir) { throw 'PIX is not installed. Get it from https://devblogs.microsoft.com/pix/download/' }
$pixtool = Join-Path $pixDir.FullName 'pixtool.exe'

$binDir = Join-Path $repo "build\$Preset\bin"
$exe = Join-Path $binDir 'WalnutRenderer.exe'
if (-not (Test-Path $exe)) { throw "Build $Preset first: $exe not found" }

if (-not $Output) {
    $Output = Join-Path $repo ("captures\WalnutRenderer_{0}_{1:yyyyMMdd_HHmmss}.wpix" -f $Kind.ToLower(), (Get-Date))
}
New-Item -ItemType Directory -Force (Split-Path -Parent $Output) | Out-Null

# The app exits on its own shortly after the capture, so pixtool and the app both finish unattended.
if ($Kind -eq 'Timing') {
    $appCommandLine = ("--exit-after-seconds={0} {1}" -f ([math]::Ceiling($DurationMs / 1000) + 5), $AppArgs).Trim()
    $pixArgs = "launch `"$exe`" --timing --command-line=`"$appCommandLine`" --working-directory=`"$binDir`" " +
               "take-new-timing-capture `"$Output`" --duration=$DurationMs"
}
else {
    $appCommandLine = "--exit-after-seconds=10 $AppArgs".Trim()
    $pixArgs = "launch `"$exe`" --command-line=`"$appCommandLine`" --working-directory=`"$binDir`" " +
               "take-capture --frames=$Frames save-capture `"$Output`""
}

Write-Host "PIX $($pixDir.Name): $Kind capture of $Preset -> $Output"
# pixtool needs the --command-line value quoted exactly like this, so pass one raw argument string.
$process = Start-Process -FilePath $pixtool -ArgumentList "--log-file=`"$env:TEMP\pixtool_walnut.log`" $pixArgs" `
    -NoNewWindow -Wait -PassThru
if ($process.ExitCode -ne 0 -or -not (Test-Path $Output)) {
    throw "pixtool failed (exit $($process.ExitCode)); see $env:TEMP\pixtool_walnut.log"
}
Write-Host ("Saved {0} ({1:N1} MB)" -f $Output, ((Get-Item $Output).Length / 1MB))
