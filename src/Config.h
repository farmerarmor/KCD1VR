#pragma once

#include <cstdint>

namespace kcdvr {

struct Config {
    float worldScale = 1.0F;
    float renderFovDegrees = 90.0F;
    float cullingFovDegrees = 110.0F;
    float hudDistance = 2.0F;
    float hudWidth = 1.8F;
    float dialogueScreenDistance = 2.0F;
    float dialogueScreenWidth = 1.5F;
    float dialogueScreenAspect = 0.0F;
    float dlssJitterScaleX = 1.0F;
    float dlssJitterScaleY = 1.0F;
    float dlssMotionScale = 1.0F;
    std::uint32_t playerViewVtableRva = 0x026C3E10;
    std::uint32_t stereoPrepareCameraRva = 0x02056F24;
    std::uint32_t cameraSetFrustumRva = 0x0030099C;
    std::uint32_t cameraUpdateFrustumRva = 0x00300B30;
    std::uint32_t flashRenderRva = 0x002E3858;
    std::uint32_t dialogueCameraVtableRva = 0x026B3820;
    std::uint32_t flashPlayerVtableRva = 0x021BC500;
    std::uint32_t flashPlayerRenderProxyVtableRva = 0x021BC4D8;
    std::uint32_t stereoLeftTextureRva = 0x02FFE9D0;
    std::uint32_t stereoRightTextureRva = 0x02FFE9C8;
    int openXrDiagnosticMode = 0;
    int dlssQuality = 2;
    int dlssRenderPreset = 0;
    std::uint32_t dlssOutputWidth = 0;
    std::uint32_t dlssOutputHeight = 0;
    bool enableHeadTracking = true;
    bool enablePositionTracking = true;
    bool enableOpenXRProjection = true;
    bool enableHudQuad = true;
    bool enableDialogueScreen = true;
    bool lockVerticalCameraInput = false;
    bool neutralizeShader1919AMotion = false;
    int doorMotionMode = 0;
    bool bypassSvoTemporalHistory = false;
    bool traceSvoResources = false;
    bool traceDoorShaderResources = false;
    bool separateSvoHistoryPerEye = true;
    bool enableDlss = false;
    bool dlssSubmitOutput = false;
    bool dlssDepthInverted = true;
    bool dlssUseRawPreSmaaColor = false;
    bool dlssReplaceNativeTemporalAa = false;
    bool dlssGpuTiming = false;
};

const Config& GetConfig();
void LoadConfig();

}  // namespace kcdvr
