param(
    [Parameter(Mandatory = $true)]
    [string]$GameRoot,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0.25, 3.0)]
    [Alias('RenderScale')]
    [double]$DisplayScale,

    [Parameter(Mandatory = $true)]
    [ValidateSet('UltraPerformance', 'Performance', 'Balanced', 'Quality', 'DLAA')]
    [string]$DLSSPreset,

    [Parameter(Mandatory = $true)]
    [ValidateSet('Default', 'J', 'K', 'L', 'M')]
    [Alias('ModelPreset')]
    [string]$DLSSRenderPreset,

    [Alias('EyeWidth')]
    [int]$OutputWidth = 0,

    [Alias('EyeHeight')]
    [int]$OutputHeight = 0
)

$ErrorActionPreference = 'Stop'
$resolvedRoot = (Resolve-Path -LiteralPath $GameRoot).Path
$binDirectory = Join-Path $resolvedRoot 'Bin\Win64'
$gameExecutable = Join-Path $binDirectory 'KingdomCome.exe'
$distribution = Join-Path $PSScriptRoot 'dist'

if (-not (Test-Path -LiteralPath $gameExecutable -PathType Leaf)) {
    throw "KingdomCome.exe was not found under '$binDirectory'."
}

$required = @(
    'dinput8.dll',
    'nvngx_dlss.dll',
    'NVIDIA-DLSS-LICENSE.txt',
    'KCD1VR.ini',
    'KCD1VR.cfg',
    'KCD1VR-dlss.cfg',
    'KCD1VR-culling-balanced.cfg',
    'KCD1VR-culling-safe.cfg',
    'KCD1VR-culling-off.cfg',
    'KCD1VR-coverage-buffer-off.cfg',
    'KCD1VR-frustum-wide.cfg',
    'KCD1VR-secondary-culling-off.cfg',
    'KCD1VR-distant-culling-safe.cfg',
    'KCD1VR-check-occlusion-off.cfg',
    'KCD1VR-object-occlusion-off.cfg',
    'KCD1VR-occlusion-worker-off.cfg',
    'KCD1VR-occlusion-margin-low.cfg',
    'KCD1VR-occlusion-margin-medium.cfg',
    'KCD1VR-occlusion-margin-high.cfg',
    'KCD1VRResolution.exe'
)
foreach ($name in $required) {
    $source = Join-Path $distribution $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Missing '$source'. Run build.ps1 first."
    }
}

$destinations = @{
    'dinput8.dll' = Join-Path $binDirectory 'dinput8.dll'
    'nvngx_dlss.dll' = Join-Path $binDirectory 'nvngx_dlss.dll'
    'NVIDIA-DLSS-LICENSE.txt' = Join-Path $binDirectory 'NVIDIA-DLSS-LICENSE.txt'
    'KCD1VR.ini' = Join-Path $binDirectory 'KCD1VR.ini'
    'KCD1VR.cfg' = Join-Path $resolvedRoot 'KCD1VR.cfg'
    'KCD1VR-dlss.cfg' = Join-Path $resolvedRoot 'KCD1VR-dlss.cfg'
    'KCD1VR-culling-balanced.cfg' = Join-Path $resolvedRoot 'KCD1VR-culling-balanced.cfg'
    'KCD1VR-culling-safe.cfg' = Join-Path $resolvedRoot 'KCD1VR-culling-safe.cfg'
    'KCD1VR-culling-off.cfg' = Join-Path $resolvedRoot 'KCD1VR-culling-off.cfg'
    'KCD1VR-coverage-buffer-off.cfg' = Join-Path $resolvedRoot 'KCD1VR-coverage-buffer-off.cfg'
    'KCD1VR-frustum-wide.cfg' = Join-Path $resolvedRoot 'KCD1VR-frustum-wide.cfg'
    'KCD1VR-secondary-culling-off.cfg' = Join-Path $resolvedRoot 'KCD1VR-secondary-culling-off.cfg'
    'KCD1VR-distant-culling-safe.cfg' = Join-Path $resolvedRoot 'KCD1VR-distant-culling-safe.cfg'
    'KCD1VR-check-occlusion-off.cfg' = Join-Path $resolvedRoot 'KCD1VR-check-occlusion-off.cfg'
    'KCD1VR-object-occlusion-off.cfg' = Join-Path $resolvedRoot 'KCD1VR-object-occlusion-off.cfg'
    'KCD1VR-occlusion-worker-off.cfg' = Join-Path $resolvedRoot 'KCD1VR-occlusion-worker-off.cfg'
    'KCD1VR-occlusion-margin-low.cfg' = Join-Path $resolvedRoot 'KCD1VR-occlusion-margin-low.cfg'
    'KCD1VR-occlusion-margin-medium.cfg' = Join-Path $resolvedRoot 'KCD1VR-occlusion-margin-medium.cfg'
    'KCD1VR-occlusion-margin-high.cfg' = Join-Path $resolvedRoot 'KCD1VR-occlusion-margin-high.cfg'
    'KCD1VRResolution.exe' = Join-Path $binDirectory 'KCD1VRResolution.exe'
}

$resolutionTool = Join-Path $distribution 'KCD1VRResolution.exe'
if (($OutputWidth -gt 0) -xor ($OutputHeight -gt 0)) {
    throw 'OutputWidth and OutputHeight must be supplied together.'
}
if ($OutputWidth -gt 0 -and $OutputHeight -gt 0) {
    $resolution = [pscustomobject]@{
        eyeWidth = $OutputWidth
        eyeHeight = $OutputHeight
        recommendedWidth = $OutputWidth
        recommendedHeight = $OutputHeight
        systemName = 'manual override'
    }
} else {
    $scaleArgument = $DisplayScale.ToString([Globalization.CultureInfo]::InvariantCulture)
    $resolutionText = & $resolutionTool --scale $scaleArgument
    if ($LASTEXITCODE -ne 0) {
        if ([Console]::IsInputRedirected) {
            throw 'Could not read the OpenXR headset resolution. Supply -OutputWidth and -OutputHeight.'
        }
        Write-Warning 'The OpenXR runtime could not provide a resolution. Enter the desired DLSS/OpenXR output resolution manually.'
        do {
            $manualWidthText = Read-Host 'Per-eye output width (for example 1344)'
            $manualWidth = 0
            $validManualWidth = [int]::TryParse($manualWidthText, [ref]$manualWidth) -and
                $manualWidth -gt 0 -and $manualWidth -le 8192
            if (-not $validManualWidth) {
                Write-Warning 'Enter a whole-number width between 1 and 8192.'
            }
        } until ($validManualWidth)
        do {
            $manualHeightText = Read-Host 'Per-eye output height (for example 1600)'
            $manualHeight = 0
            $validManualHeight = [int]::TryParse($manualHeightText, [ref]$manualHeight) -and
                $manualHeight -gt 0 -and $manualHeight -le 8192
            if (-not $validManualHeight) {
                Write-Warning 'Enter a whole-number height between 1 and 8192.'
            }
        } until ($validManualHeight)
        $resolution = [pscustomobject]@{
            eyeWidth = $manualWidth
            eyeHeight = $manualHeight
            recommendedWidth = $manualWidth
            recommendedHeight = $manualHeight
            systemName = 'manual fallback'
        }
    } else {
        $resolution = $resolutionText | ConvertFrom-Json
    }
}
$selectedOutputWidth = [int]$resolution.eyeWidth
$selectedOutputHeight = [int]$resolution.eyeHeight
if ($selectedOutputWidth -le 0 -or $selectedOutputHeight -le 0 -or
    $selectedOutputWidth -gt 8192 -or $selectedOutputHeight -gt 8192) {
    throw 'The selected per-eye output resolution is invalid.'
}

# DLSS presets select an internal render fraction of the requested display
# resolution. CryEngine needs that render size before it creates D3D11, so the
# installer writes the preset-derived dimensions to its startup CVars.
$dlssSettings = switch ($DLSSPreset) {
    'UltraPerformance' { @{ Quality = 3; Ratio = 1.0 / 3.0; Name = 'Ultra Performance' } }
    'Performance'      { @{ Quality = 0; Ratio = 0.5;       Name = 'Performance' } }
    'Balanced'         { @{ Quality = 1; Ratio = 0.58;      Name = 'Balanced' } }
    'Quality'          { @{ Quality = 2; Ratio = 2.0 / 3.0; Name = 'Quality' } }
    'DLAA'             { @{ Quality = 5; Ratio = 1.0;       Name = 'DLAA' } }
}
$selectedRenderPreset = if ($DLSSRenderPreset -eq 'Default') {
    'Default'
} else {
    $DLSSRenderPreset.ToUpperInvariant()
}
function Get-RenderDimension([int]$Dimension, [double]$Ratio) {
    return [int][math]::Round($Dimension * $Ratio, 0,
        [MidpointRounding]::AwayFromZero)
}
$configuredEyeWidth = Get-RenderDimension $selectedOutputWidth $dlssSettings.Ratio
$configuredEyeHeight = Get-RenderDimension $selectedOutputHeight $dlssSettings.Ratio
if ($configuredEyeWidth -le 0 -or $configuredEyeHeight -le 0 -or
    $configuredEyeWidth -gt 8192 -or $configuredEyeHeight -gt 8192) {
    throw 'The DLSS preset produced an invalid per-eye render resolution.'
}

foreach ($name in $required) {
    $source = Join-Path $distribution $name
    $destination = $destinations[$name]
    Copy-Item -LiteralPath $source -Destination $destination -Force
    Write-Host "Installed: $destination"
}

$rootConfigPath = $destinations['KCD1VR.cfg']
$rootConfig = [IO.File]::ReadAllText($rootConfigPath)
$rootConfig = [regex]::Replace($rootConfig, '(?m)^\s*r_Width\s*=.*$', "r_Width = $configuredEyeWidth")
$rootConfig = [regex]::Replace($rootConfig, '(?m)^\s*r_Height\s*=.*$', "r_Height = $configuredEyeHeight")
[IO.File]::WriteAllText($rootConfigPath, $rootConfig, [Text.UTF8Encoding]::new($false))

$iniPath = $destinations['KCD1VR.ini']
$ini = [IO.File]::ReadAllText($iniPath)
$ini = [regex]::Replace($ini, '(?m)^\s*DLSSQuality\s*=.*$', "DLSSQuality=$($dlssSettings.Quality)")
$ini = [regex]::Replace($ini, '(?m)^\s*DLSSRenderPreset\s*=.*$', "DLSSRenderPreset=$selectedRenderPreset")
$ini = [regex]::Replace($ini, '(?m)^\s*DLSSOutputWidth\s*=.*$', "DLSSOutputWidth=$selectedOutputWidth")
$ini = [regex]::Replace($ini, '(?m)^\s*DLSSOutputHeight\s*=.*$', "DLSSOutputHeight=$selectedOutputHeight")
[IO.File]::WriteAllText($iniPath, $ini, [Text.UTF8Encoding]::new($false))

# r_StereoDevice and sys_vr_support are read before the renderer starts. KCD's
# +exec command runs too late for those CVars, so keep an installer-owned block
# at the end of user.cfg. Unrelated user settings are preserved.
$userConfig = Join-Path $resolvedRoot 'user.cfg'
$managedBlock = @"
-- BEGIN KCD1VR MANAGED SETTINGS
r_StereoDevice = 1
sys_vr_support = 0
r_StereoMode = 1
r_StereoOutput = 4
r_StereoEyeDist = 0.064
r_StereoFlipEyes = 0
r_Width = $configuredEyeWidth
r_Height = $configuredEyeHeight
r_Fullscreen = 0
r_VSync = 0
-- END KCD1VR MANAGED SETTINGS
"@

$existingConfig = ''
if (Test-Path -LiteralPath $userConfig -PathType Leaf) {
    $loadedConfig = Get-Content -LiteralPath $userConfig -Raw
    if ($null -ne $loadedConfig) {
        $existingConfig = $loadedConfig
    }
    $managedPattern = '(?ms)^\s*-- BEGIN KCD1VR MANAGED SETTINGS\s*\r?\n.*?^\s*-- END KCD1VR MANAGED SETTINGS\s*(?:\r?\n)?'
    $existingConfig = [regex]::Replace($existingConfig, $managedPattern, '')
}

if ($existingConfig.Length -gt 0 -and -not $existingConfig.EndsWith("`n")) {
    $existingConfig += "`r`n"
}
$updatedConfig = $existingConfig + $managedBlock + "`r`n"
[IO.File]::WriteAllText($userConfig, $updatedConfig, [Text.UTF8Encoding]::new($false))
Write-Host "Updated startup settings: $userConfig"

Write-Host ''
Write-Host 'Use this launch option: +exec KCD1VR.cfg +exec KCD1VR-dlss.cfg'
Write-Host "OpenXR resolution: $($resolution.systemName), recommended $($resolution.recommendedWidth)x$($resolution.recommendedHeight) per eye"
Write-Host "Selected DLSS output: ${selectedOutputWidth}x${selectedOutputHeight} per eye at ${DisplayScale}x display scale"
Write-Host "DLSS preset: $($dlssSettings.Name) (quality $($dlssSettings.Quality), $([math]::Round($dlssSettings.Ratio * 100))% render scale)"
Write-Host "DLSS model preset: $selectedRenderPreset"
Write-Host "Automatically configured game render size: ${configuredEyeWidth}x${configuredEyeHeight} per eye"
Write-Host 'Fully exit KCD before testing; the stereo device is startup-only.'
Write-Host 'KCD1VR.log will be written beside KingdomCome.exe.'
