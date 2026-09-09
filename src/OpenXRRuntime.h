#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>

#include <array>
#include <mutex>
#include <vector>

namespace kcdvr {

class OpenXRRuntime final {
public:
    static OpenXRRuntime& Instance();

    bool Attach(ID3D11Device* device, IDXGISwapChain* swapChain);
    void OnPresent(IDXGISwapChain* presentingSwapChain);
    void OnResize(IDXGISwapChain* resizedSwapChain);
    void OnHudRenderBegin(ID3D11Texture2D* leftEye, ID3D11Texture2D* rightEye);
    void SetOpenXRProjectionEnabled(bool enabled);
    void SetHudSeparationEnabled(bool enabled);
    void Shutdown();

private:
    bool CreateInstance();
    bool CreateSession();
    bool EnsureSwapchain(ID3D11Texture2D* backBuffer);
    bool EnsureHudSwapchain(DXGI_FORMAT format);
    bool IsSupportedStereoBackbuffer(const D3D11_TEXTURE2D_DESC& description) const;
    void ResetObservedEyeTextures();
    void DestroySwapchain();
    void DestroyHudSwapchain();
    void PollEvents();
    void HandleSessionState(XrSessionState state);
    void SubmitPreparedFrame(ID3D11Texture2D* backBuffer, bool submitLayers);
    void PrepareNextFrame();
    void UpdateTrackingPose();
    bool Check(XrResult result, const char* operation) const;

    std::mutex m_mutex;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_space = XR_NULL_HANDLE;
    XrSpace m_viewSpace = XR_NULL_HANDLE;
    XrSwapchain m_stereoSwapchain = XR_NULL_HANDLE;
    XrSwapchain m_hudSwapchain = XR_NULL_HANDLE;
    XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;
    bool m_sessionRunning = false;
    bool m_framePrepared = false;
    bool m_shouldRender = false;
    bool m_recreateSwapchain = false;
    bool m_loggedRejectedBackbuffer = false;
    bool m_useOpenXrProjection = false;
    bool m_hudSeparationEnabled = false;
    bool m_hudSeparatedThisFrame = false;
    bool m_dialogueScreenThisFrame = false;
    bool m_dlssSwapchainActive = false;
    bool m_dlssHasValidFrame = false;
    bool m_loggedDlssFrameReuse = false;
    bool m_loggedDlssOffscreenProbe = false;
    bool m_loggedHudCapture = false;
    bool m_loggedRejectedHudEyes = false;
    bool m_loggedDialogueScreen = false;
    std::uint32_t m_lastPresentWidth = 0;
    std::uint32_t m_lastPresentHeight = 0;
    XrTime m_displayTime = 0;

    std::uint32_t m_eyeWidth = 0;
    std::uint32_t m_eyeHeight = 0;
    DXGI_FORMAT m_swapchainFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_hudSwapchainFormat = DXGI_FORMAT_UNKNOWN;
    std::uint32_t m_hudSwapchainWidth = 0;
    std::uint32_t m_hudSwapchainHeight = 0;
    std::array<XrView, 2> m_views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    std::vector<XrSwapchainImageD3D11KHR> m_swapchainImages;
    std::vector<XrSwapchainImageD3D11KHR> m_hudSwapchainImages;
    std::uint32_t m_engineEyeWidth = 0;
    std::uint32_t m_engineEyeHeight = 0;
    DXGI_FORMAT m_engineEyeFormat = DXGI_FORMAT_UNKNOWN;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 2> m_engineEyeTextures;
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, 2> m_engineEyeRenderTargets;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 2> m_worldEyeTextures;

    PFN_xrGetD3D11GraphicsRequirementsKHR m_getD3D11Requirements = nullptr;
};

}  // namespace kcdvr
