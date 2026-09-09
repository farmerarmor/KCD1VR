param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$sourceDirectory = $PSScriptRoot
$buildDirectory = Join-Path $sourceDirectory 'build-vs'
$dlssDirectory = Join-Path $sourceDirectory '.deps\NVIDIA-DLSS'
$dlssCommit = 'a291cc7d2cc642a51566f3dfd5376f635cd1b284'
$dlssLibrary = Join-Path $dlssDirectory 'lib\Windows_x86_64\x64\nvsdk_ngx_d.lib'
$dlssHeader = Join-Path $dlssDirectory 'include\nvsdk_ngx.h'

if (-not (Test-Path -LiteralPath $dlssLibrary -PathType Leaf) -or
    -not (Test-Path -LiteralPath $dlssHeader -PathType Leaf)) {
    if (Test-Path -LiteralPath $dlssDirectory) {
        throw "The existing DLSS SDK folder is incomplete: '$dlssDirectory'. Remove or repair it before building."
    }

    New-Item -ItemType Directory -Path (Split-Path -Parent $dlssDirectory) -Force | Out-Null
    git clone --filter=blob:none --no-checkout https://github.com/NVIDIA/DLSS.git $dlssDirectory
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    git -C $dlssDirectory checkout --detach $dlssCommit
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}

$visualStudioDirectory = (& $vswhere -latest -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1)
if ([string]::IsNullOrWhiteSpace($visualStudioDirectory)) {
    throw 'Visual Studio 2022 with the Desktop development with C++ workload was not found.'
}

$devShell = Join-Path $visualStudioDirectory 'Common7\Tools\Launch-VsDevShell.ps1'

if (-not (Test-Path -LiteralPath $devShell)) {
    throw "Visual Studio developer shell was not found under '$visualStudioDirectory'."
}

& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation

cmake -S $sourceDirectory -B $buildDirectory -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=$Configuration
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $buildDirectory --target KCD1VR KCD1VRResolution
exit $LASTEXITCODE
