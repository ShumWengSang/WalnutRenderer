# Turns the current PowerShell session into an x64 Visual Studio developer shell (cl, link, cmake, ninja).
# Used by the "Developer PowerShell (x64)" terminal profile in .vscode/settings.json.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Write-Warning 'vswhere.exe not found; install Visual Studio Build Tools with the C++ workload.'
    return
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    Write-Warning 'No Visual Studio installation with the MSVC x64 toolset was found.'
    return
}

Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

# Prefer the standalone CMake install (newer than the one bundled with Visual Studio) when present.
$standaloneCMake = Join-Path $env:ProgramFiles 'CMake\bin'
if (Test-Path $standaloneCMake) {
    $env:Path = "$standaloneCMake;$env:Path"
}

Write-Host "VS dev shell ready: $(Split-Path $vsPath -Leaf), $((cmake --version | Select-Object -First 1))"
