#include "Config.h"

#include "Log.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>

namespace kcdvr {
namespace {

Config g_config;

float ReadFloat(const wchar_t* key, float fallback, const std::filesystem::path& path)
{
    std::array<wchar_t, 64> value{};
    GetPrivateProfileStringW(L"KCD1VR", key, L"", value.data(),
                             static_cast<DWORD>(value.size()), path.c_str());
    if (value[0] == L'\0') {
        return fallback;
    }
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(value.data(), &end);
    return end != value.data() ? parsed : fallback;
}

int ReadDlssRenderPreset(const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(L"KCD1VR", L"DLSSRenderPreset", L"Default",
                             value.data(), static_cast<DWORD>(value.size()),
                             path.c_str());
    if (value[0] == L'\0' || value[1] != L'\0') {
        return 0;
    }
    switch (std::towupper(value[0])) {
    case L'J': return 10;
    case L'K': return 11;
    case L'L': return 12;
    case L'M': return 13;
    default: return 0;
    }
}

}  // namespace

const Config& GetConfig()
{
    return g_config;
}

void LoadConfig()
{
    const auto path = ModuleDirectory() / L"KCD1VR.ini";
    g_config.worldScale = std::clamp(ReadFloat(L"WorldScale", 1.0F, path), 0.01F, 100.0F);
    g_config.renderFovDegrees =
        std::clamp(ReadFloat(L"RenderFovDegrees", 90.0F, path), 45.0F, 140.0F);
    g_config.cullingFovDegrees =
        std::clamp(ReadFloat(L"CullingFovDegrees", 110.0F, path), 0.0F, 170.0F);
    g_config.hudDistance =
        std::clamp(ReadFloat(L"HudDistance", 2.0F, path), 0.25F, 20.0F);
    g_config.hudWidth =
        std::clamp(ReadFloat(L"HudWidth", 1.8F, path), 0.25F, 20.0F);
    g_config.dialogueScreenDistance =
        std::clamp(ReadFloat(L"DialogueScreenDistance", 2.0F, path), 0.25F, 20.0F);
    g_config.dialogueScreenWidth =
        std::clamp(ReadFloat(L"DialogueScreenWidth", 1.5F, path), 0.25F, 20.0F);
    g_config.dialogueScreenAspect =
        std::clamp(ReadFloat(L"DialogueScreenAspect", 0.0F, path), 0.0F, 4.0F);
    g_config.dlssJitterScaleX =
        std::clamp(ReadFloat(L"DLSSJitterScaleX", 1.0F, path), -4.0F, 4.0F);
    g_config.dlssJitterScaleY =
        std::clamp(ReadFloat(L"DLSSJitterScaleY", 1.0F, path), -4.0F, 4.0F);
    g_config.dlssMotionScale =
        std::clamp(ReadFloat(L"DLSSMotionScale", 1.0F, path), 0.0F, 4.0F);
    g_config.openXrDiagnosticMode = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            L"KCD1VR", L"OpenXRDiagnosticMode", 0, path.c_str())),
        0, 2);
    g_config.enableHeadTracking =
        GetPrivateProfileIntW(L"KCD1VR", L"HeadTracking", 1, path.c_str()) != 0;
    g_config.enablePositionTracking =
        GetPrivateProfileIntW(L"KCD1VR", L"PositionTracking", 1, path.c_str()) != 0;
    g_config.enableOpenXRProjection =
        GetPrivateProfileIntW(L"KCD1VR", L"OpenXRProjection", 1, path.c_str()) != 0;
    g_config.useOpenXrSrgbSwapchains =
        GetPrivateProfileIntW(L"KCD1VR", L"OpenXRSrgbSwapchains", 1,
                              path.c_str()) != 0;
    g_config.enableHudQuad =
        GetPrivateProfileIntW(L"KCD1VR", L"HudQuad", 1, path.c_str()) != 0;
    g_config.enableDialogueScreen =
        GetPrivateProfileIntW(L"KCD1VR", L"DialogueScreen", 1, path.c_str()) != 0;
    g_config.lockVerticalCameraInput =
        GetPrivateProfileIntW(L"KCD1VR", L"LockVerticalCameraInput", 0,
                              path.c_str()) != 0;
    g_config.neutralizeShader1919AMotion =
        GetPrivateProfileIntW(L"KCD1VR", L"NeutralizeShader1919AMotion", 0,
                              path.c_str()) != 0;
    g_config.doorMotionMode = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            L"KCD1VR", L"DoorMotionMode", 0, path.c_str())),
        0, 2);
    g_config.bypassSvoTemporalHistory =
        GetPrivateProfileIntW(L"KCD1VR", L"BypassSvoTemporalHistory", 0,
                              path.c_str()) != 0;
    g_config.traceSvoResources =
        GetPrivateProfileIntW(L"KCD1VR", L"TraceSvoResources", 0,
                              path.c_str()) != 0;
    g_config.traceDoorShaderResources =
        GetPrivateProfileIntW(L"KCD1VR", L"TraceDoorShaderResources", 0,
                              path.c_str()) != 0;
    g_config.separateSvoHistoryPerEye =
        GetPrivateProfileIntW(L"KCD1VR", L"SeparateSvoHistoryPerEye", 1,
                              path.c_str()) != 0;
    g_config.enableDlss =
        GetPrivateProfileIntW(L"KCD1VR", L"DLSS", 0, path.c_str()) != 0;
    g_config.dlssSubmitOutput =
        GetPrivateProfileIntW(L"KCD1VR", L"DLSSSubmit", 0, path.c_str()) != 0;
    g_config.dlssQuality = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            L"KCD1VR", L"DLSSQuality", 2, path.c_str())),
        0, 5);
    g_config.dlssRenderPreset = ReadDlssRenderPreset(path);
    g_config.dlssOutputWidth = static_cast<std::uint32_t>(std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            L"KCD1VR", L"DLSSOutputWidth", 0, path.c_str())),
        0, 8192));
    g_config.dlssOutputHeight = static_cast<std::uint32_t>(std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            L"KCD1VR", L"DLSSOutputHeight", 0, path.c_str())),
        0, 8192));
    g_config.dlssDepthInverted =
        GetPrivateProfileIntW(L"KCD1VR", L"DLSSDepthInverted", 1,
                              path.c_str()) != 0;
    g_config.dlssUseRawPreSmaaColor =
        GetPrivateProfileIntW(L"KCD1VR", L"DLSSUseRawPreSmaaColor", 0,
                              path.c_str()) != 0;
    g_config.dlssReplaceNativeTemporalAa =
        GetPrivateProfileIntW(L"KCD1VR", L"DLSSReplaceNativeTemporalAA", 0,
                              path.c_str()) != 0;
    g_config.dlssGpuTiming =
        GetPrivateProfileIntW(L"KCD1VR", L"DLSSGpuTiming", 0,
                              path.c_str()) != 0;
    g_config.playerViewVtableRva = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"KCD1VR", L"PlayerViewVtableRva", 0x026C3E10,
                              path.c_str()));
    g_config.stereoPrepareCameraRva = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"KCD1VR", L"StereoPrepareCameraRva", 0x02056F24,
                              path.c_str()));
    g_config.cameraSetFrustumRva = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"KCD1VR", L"CameraSetFrustumRva", 0x0030099C,
                              path.c_str()));
    g_config.cameraUpdateFrustumRva = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"KCD1VR", L"CameraUpdateFrustumRva", 0x00300B30,
                              path.c_str()));
    g_config.flashRenderRva = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"KCD1VR", L"FlashRenderRva", 0x002E3858,
                              path.c_str()));
    g_config.dialogueCameraVtableRva = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"KCD1VR", L"DialogueCameraVtableRva", 0x026B3820,
                              path.c_str()));
    g_config.flashPlayerVtableRva = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"KCD1VR", L"FlashPlayerVtableRva", 0x021BC500,
                              path.c_str()));
    g_config.flashPlayerRenderProxyVtableRva = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"KCD1VR", L"FlashPlayerRenderProxyVtableRva",
                              0x021BC4D8, path.c_str()));
    g_config.stereoLeftTextureRva = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"KCD1VR", L"StereoLeftTextureRva", 0x02FFE9D0,
                              path.c_str()));
    g_config.stereoRightTextureRva = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"KCD1VR", L"StereoRightTextureRva", 0x02FFE9C8,
                              path.c_str()));
    Log("Config: worldScale=%.3f, fallbackFov=%.1f, cullingFov=%.1f, xrDiagnosticMode=%d, headTracking=%d, positionTracking=%d, OpenXRProjection=%d, OpenXRSrgbSwapchains=%d, HudQuad=%d (%.2fm wide at %.2fm), dialogueScreen=%d (%.2fm wide at %.2fm, aspect %.3f), lockVerticalInput=%d, neutralize1919A=%d, doorMotionMode=%d, bypassSvoHistory=%d, traceSvo=%d, traceDoor=%d, separateSvoHistory=%d, DLSS=%d submit=%d quality=%d renderPreset=%d output=%ux%u depthInverted=%d rawPreSmaa=%d motionScale=%.3f replaceNativeTaa=%d gpuTiming=%d jitterScale=(%.2f,%.2f), cameraVtableRva=0x%08X, prepareCameraRva=0x%08X, dialogueCameraVtableRva=0x%08X",
        g_config.worldScale, g_config.renderFovDegrees,
        g_config.cullingFovDegrees,
        g_config.openXrDiagnosticMode,
        g_config.enableHeadTracking ? 1 : 0,
        g_config.enablePositionTracking ? 1 : 0,
        g_config.enableOpenXRProjection ? 1 : 0,
        g_config.useOpenXrSrgbSwapchains ? 1 : 0,
        g_config.enableHudQuad ? 1 : 0, g_config.hudWidth, g_config.hudDistance,
        g_config.enableDialogueScreen ? 1 : 0, g_config.dialogueScreenWidth,
        g_config.dialogueScreenDistance, g_config.dialogueScreenAspect,
        g_config.lockVerticalCameraInput ? 1 : 0,
        g_config.neutralizeShader1919AMotion ? 1 : 0,
        g_config.doorMotionMode,
        g_config.bypassSvoTemporalHistory ? 1 : 0,
        g_config.traceSvoResources ? 1 : 0,
        g_config.traceDoorShaderResources ? 1 : 0,
        g_config.separateSvoHistoryPerEye ? 1 : 0,
        g_config.enableDlss ? 1 : 0, g_config.dlssSubmitOutput ? 1 : 0,
        g_config.dlssQuality, g_config.dlssRenderPreset,
        g_config.dlssOutputWidth, g_config.dlssOutputHeight,
        g_config.dlssDepthInverted ? 1 : 0,
        g_config.dlssUseRawPreSmaaColor ? 1 : 0,
        g_config.dlssMotionScale,
        g_config.dlssReplaceNativeTemporalAa ? 1 : 0,
        g_config.dlssGpuTiming ? 1 : 0,
        g_config.dlssJitterScaleX, g_config.dlssJitterScaleY,
        g_config.playerViewVtableRva, g_config.stereoPrepareCameraRva,
        g_config.dialogueCameraVtableRva);
}

}  // namespace kcdvr
