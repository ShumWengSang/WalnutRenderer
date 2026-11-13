<#
.SYNOPSIS
    Compares two folders of PNG screenshots with the same file names (see screenshot-matrix.ps1).

.EXAMPLE
    .\tools\compare-images.ps1 captures\matrix-before captures\matrix-after
    .\tools\compare-images.ps1 before after -Tolerance 2   # allow differences up to 2 of 255 per channel

.NOTES
    Prints, per image: how many pixels differ, the largest channel difference, and the mean. Exits 1 if any
    image differs by more than -Tolerance (default 0: bit-identical) or is missing.
#>
param(
    [Parameter(Mandatory)] [string] $Before,
    [Parameter(Mandatory)] [string] $After,
    [int] $Tolerance = 0
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Read-Pixels([string] $path) {
    $bitmap = [System.Drawing.Bitmap]::FromFile((Resolve-Path $path))
    try {
        $rect = New-Object System.Drawing.Rectangle 0, 0, $bitmap.Width, $bitmap.Height
        $data = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $bytes = New-Object byte[] ($data.Stride * $bitmap.Height)
        [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
        $bitmap.UnlockBits($data)
        return @{ Width = $bitmap.Width; Height = $bitmap.Height; Bytes = $bytes }
    } finally { $bitmap.Dispose() }
}

# The per-pixel loop in C#: PowerShell would take minutes per image.
Add-Type -TypeDefinition @'
public static class ImageDiff {
    public static long[] Compare(byte[] a, byte[] b) {   // { differing pixels, max channel diff, sum of max diffs }
        long differing = 0, max = 0, sum = 0;
        for (int i = 0; i < a.Length; i += 4) {
            int d = 0;
            for (int c = 0; c < 3; ++c) { int x = a[i + c] - b[i + c]; if (x < 0) x = -x; if (x > d) d = x; }
            if (d > 0) ++differing;
            if (d > max) max = d;
            sum += d;
        }
        return new long[] { differing, max, sum };
    }
}
'@

$failed = 0
$files = Get-ChildItem (Join-Path $Before '*.png') | Sort-Object Name
foreach ($file in $files) {
    $other = Join-Path $After $file.Name
    if (-not (Test-Path $other)) { Write-Host ("MISSING {0}" -f $file.Name) -ForegroundColor Red; $failed++; continue }
    $a = Read-Pixels $file.FullName
    $b = Read-Pixels $other
    if ($a.Width -ne $b.Width -or $a.Height -ne $b.Height) { Write-Host "SIZE    $($file.Name)" -ForegroundColor Red; $failed++; continue }
    $r = [ImageDiff]::Compare($a.Bytes, $b.Bytes)
    $pixels = $a.Width * $a.Height
    $status = if ($r[1] -le $Tolerance) { 'same' } else { $failed++; 'DIFF' }
    $color = if ($status -eq 'same') { 'Gray' } else { 'Red' }
    $line = "{0,-5} {1,-28} {2,8} px differ  max {3,3}  mean {4:F4}" -f $status, $file.Name, $r[0], $r[1], ($r[2] / $pixels)
    if ($status -eq 'same') { Write-Verbose $line } else { Write-Host $line -ForegroundColor $color }
}
Write-Host "$($files.Count) images, $failed over tolerance $Tolerance"
if ($failed) { exit 1 }
