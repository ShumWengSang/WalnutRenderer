<#
.SYNOPSIS
    Downloads glTF sample models into <repo>\assets (git-ignored).

.DESCRIPTION
    Does a sparse, shallow fetch of KhronosGroup/glTF-Sample-Assets at a pinned commit, so only the
    requested models' glTF folders are downloaded (not the whole multi-GB repository). Each model ends
    up in assets\<Model>\ (for example assets\Sponza\Sponza.gltf), next to its README and LICENSE.

.EXAMPLE
    .\tools\fetch-assets.ps1                              # Sponza
    .\tools\fetch-assets.ps1 -Models Sponza,DamagedHelmet
    .\tools\fetch-assets.ps1 -Force                       # download again even if present
#>
param(
    [string[]] $Models = @('Sponza'),
    [switch] $Force
)
$ErrorActionPreference = 'Stop'

$repoUrl = 'https://github.com/KhronosGroup/glTF-Sample-Assets.git'
$commit = '8f492390ddc7a2ae221a1369ef560589cf8c70fb'

$assets = Join-Path (Split-Path -Parent $PSScriptRoot) 'assets'
$wanted = @($Models | Where-Object { $Force -or -not (Test-Path (Join-Path $assets $_)) })
if ($wanted.Count -eq 0) {
    Write-Host "Already present in ${assets}: $($Models -join ', ') (use -Force to download again)"
    return
}

function Invoke-Git {
    git @args
    if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed (exit $LASTEXITCODE)" }
}

$download = Join-Path $assets '.download'
if (Test-Path $download) { Remove-Item $download -Recurse -Force }
New-Item -ItemType Directory -Force $download | Out-Null
try {
    Invoke-Git init -q $download
    Invoke-Git -C $download remote add origin $repoUrl
    # Only these paths are checked out; with --filter=blob:none, only their file contents are downloaded.
    $patterns = foreach ($model in $wanted) { "/Models/$model/glTF/"; "/Models/$model/*.md" }
    Invoke-Git -C $download sparse-checkout set --no-cone @patterns
    Write-Host "Fetching $($wanted -join ', ') from glTF-Sample-Assets @ $($commit.Substring(0, 8))..."
    Invoke-Git -C $download fetch -q --depth 1 --filter=blob:none origin $commit
    Invoke-Git -C $download checkout -q FETCH_HEAD

    foreach ($model in $wanted) {
        $source = Join-Path $download "Models\$model"
        if (-not (Test-Path (Join-Path $source 'glTF'))) { throw "Model '$model' has no glTF folder in glTF-Sample-Assets" }
        $target = Join-Path $assets $model
        if (Test-Path $target) { Remove-Item $target -Recurse -Force }
        Copy-Item (Join-Path $source 'glTF') $target -Recurse
        Copy-Item (Join-Path $source '*.md') $target
        $files = Get-ChildItem $target -Recurse -File
        Write-Host ("  {0}: {1} files, {2:N1} MB -> {3}" -f $model, $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB), $target)
    }
}
finally {
    Remove-Item $download -Recurse -Force -ErrorAction SilentlyContinue
}
