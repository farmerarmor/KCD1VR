#include "OpenXRRuntime.h"

#include "Config.h"
#include "DlssUpscaler.h"
#include "KcdCamera.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace kcdvr {
namespace {

bool LuidMatches(const LUID& left, const LUID& right)
{
    return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

XrQuaternionf AverageOrientation(const XrQuaternionf& a, XrQuaternionf b)
{
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0F) {
        b = {-b.x, -b.y, -b.z, -b.w};
    }
    XrQuaternionf result{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    const float length = std::sqrt(result.x * result.x + result.y * result.y +
                                   result.z * result.z + result.w * result.w);
    if (length > 0.000001F) {
        result.x /= length;
        result.y /= length;
        result.z /= length;
        result.w /= length;
    } else {
        result = {0.0F, 0.0F, 0.0F, 1.0F};
    }
    return result;
}

DXGI_FORMAT SrgbVariant(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    default:
        return format;
    }
}

DXGI_FORMAT SelectOpenXrColorFormat(DXGI_FORMAT sourceFormat,
                                    const std::vector<std::int64_t>& supportedFormats,
                                    bool useSrgb)
{
    const DXGI_FORMAT preferred = useSrgb ? SrgbVariant(sourceFormat) : sourceFormat;
    const auto isSupported = [&supportedFormats](DXGI_FORMAT format) {
        return std::find(supportedFormats.begin(), supportedFormats.end(),
                         static_cast<std::int64_t>(format)) != supportedFormats.end();
    };
    if (isSupported(preferred)) {
        return preferred;
    }
    return isSupported(sourceFormat) ? sourceFormat : DXGI_FORMAT_UNKNOWN;
}

}  // namespace

OpenXRRuntime& OpenXRRuntime::Instance()
{
    static OpenXRRuntime runtime;
    return runtime;
}

bool OpenXRRuntime::IsSupportedStereoBackbuffer(
    const D3D11_TEXTURE2D_DESC& description) const
{
    // CryEngine exposes its two native eye render targets through the stereo
    // renderer. Match the final side-by-side buffer against those resources
    // instead of duplicating an installer-selected pixel size in the INI.
    return m_engineEyeWidth != 0 && m_engineEyeHeight != 0 &&
           description.Width == m_engineEyeWidth * 2 &&
           description.Height == m_engineEyeHeight &&
           description.Format == m_engineEyeFormat &&
           description.SampleDesc.Count == 1;
}

void OpenXRRuntime::ResetObservedEyeTextures()
{
    m_engineEyeWidth = 0;
    m_engineEyeHeight = 0;
    m_engineEyeFormat = DXGI_FORMAT_UNKNOWN;
    for (auto& renderTarget : m_engineEyeRenderTargets) {
        renderTarget.Reset();
    }
    for (auto& texture : m_engineEyeTextures) {
        texture.Reset();
    }
    m_dialogueScreenThisFrame = false;
}

bool OpenXRRuntime::Check(XrResult result, const char* operation) const
{
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    char resultName[XR_MAX_RESULT_STRING_SIZE]{};
    if (m_instance != XR_NULL_HANDLE) {
        xrResultToString(m_instance, result, resultName);
    }
    Log("OpenXR error while %s: %d %s", operation, result, resultName);
    return false;
}

bool OpenXRRuntime::CreateInstance()
{
    if (m_instance != XR_NULL_HANDLE) {
        return true;
    }

    const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy_s(createInfo.applicationInfo.applicationName, "KCD1VR",
              XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    strncpy_s(createInfo.applicationInfo.engineName, "CryEngine/KCD1",
              XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.engineVersion = 1;
    // KCD1VR uses only OpenXR 1.0 core commands plus XR_KHR_D3D11_enable.
    // Requesting the header's current (1.1) version can make an otherwise
    // compatible OpenXR 1.0 runtime reject instance creation.
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = extensions;

    if (!Check(xrCreateInstance(&createInfo, &m_instance), "creating instance")) {
        return false;
    }

    XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &instanceProperties))) {
        Log("OpenXR runtime: %s %u.%u.%u; requested API 1.0.0",
            instanceProperties.runtimeName,
            XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
            XR_VERSION_MINOR(instanceProperties.runtimeVersion),
            XR_VERSION_PATCH(instanceProperties.runtimeVersion));
    }

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(m_instance, &systemInfo, &m_systemId), "getting HMD system")) {
        return false;
    }

    if (!Check(xrGetInstanceProcAddr(
                   m_instance, "xrGetD3D11GraphicsRequirementsKHR",
                   reinterpret_cast<PFN_xrVoidFunction*>(&m_getD3D11Requirements)),
               "loading D3D11 requirements function")) {
        return false;
    }
    return true;
}

bool OpenXRRuntime::CreateSession()
{
    if (m_session != XR_NULL_HANDLE) {
        return true;
    }
    if (!CreateInstance() || !m_device) {
        return false;
    }

    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Check(m_getD3D11Requirements(m_instance, m_systemId, &requirements),
               "getting D3D11 graphics requirements")) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDescription{};
    if (FAILED(m_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&adapterDescription))) {
        Log("Unable to query the game's DXGI adapter");
        return false;
    }
    if (!LuidMatches(adapterDescription.AdapterLuid, requirements.adapterLuid)) {
        Log("OpenXR runtime requires a different GPU adapter; aborting VR session");
        return false;
    }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = m_device.Get();
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = m_systemId;
    if (!Check(xrCreateSession(m_instance, &sessionInfo, &m_session), "creating session")) {
        return false;
    }

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0F;
    if (!Check(xrCreateReferenceSpace(m_session, &spaceInfo, &m_space),
               "creating local reference space")) {
        return false;
    }
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(m_session, &spaceInfo, &m_viewSpace),
               "creating view reference space")) {
        return false;
    }
    Log("OpenXR session created on %ls", adapterDescription.Description);
    return true;
}

bool OpenXRRuntime::Attach(ID3D11Device* device, IDXGISwapChain* swapChain)
{
    std::scoped_lock lock(m_mutex);
    if (!device || !swapChain) {
        return false;
    }

    if (m_device.Get() != device) {
        Shutdown();
        m_device = device;
        m_device->GetImmediateContext(&m_context);
        DlssUpscaler::Instance().Attach(device);
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> candidateBuffer;
    D3D11_TEXTURE2D_DESC candidateDescription{};
    const bool candidateReadable =
        SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&candidateBuffer)));
    if (candidateReadable) {
        candidateBuffer->GetDesc(&candidateDescription);
    }
    const bool candidateMatches = candidateReadable &&
                                  IsSupportedStereoBackbuffer(candidateDescription);
    if (!m_swapChain || candidateMatches) {
        m_swapChain = swapChain;
        Log("Selected main swapchain %p (%ux%u)", swapChain,
            candidateDescription.Width, candidateDescription.Height);
    } else {
        Log("Leaving main swapchain unchanged; candidate %p is %ux%u", swapChain,
            candidateDescription.Width, candidateDescription.Height);
    }
    return CreateSession();
}

void OpenXRRuntime::PollEvents()
{
    if (m_instance == XR_NULL_HANDLE) {
        return;
    }
    while (true) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult result = xrPollEvent(m_instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) {
            break;
        }
        if (!Check(result, "polling events")) {
            break;
        }
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            HandleSessionState(changed->state);
        }
    }
}

void OpenXRRuntime::HandleSessionState(XrSessionState state)
{
    m_sessionState = state;
    Log("OpenXR session state changed to %d", static_cast<int>(state));
    if (state == XR_SESSION_STATE_READY && !m_sessionRunning) {
        XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (Check(xrBeginSession(m_session, &beginInfo), "beginning session")) {
            m_sessionRunning = true;
        }
    } else if (state == XR_SESSION_STATE_STOPPING && m_sessionRunning) {
        if (m_framePrepared) {
            XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = m_displayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            xrEndFrame(m_session, &endInfo);
            m_framePrepared = false;
        }
        xrEndSession(m_session);
        m_sessionRunning = false;
    }
}

bool OpenXRRuntime::EnsureSwapchain(ID3D11Texture2D* backBuffer)
{
    D3D11_TEXTURE2D_DESC backBufferDescription{};
    backBuffer->GetDesc(&backBufferDescription);
    if (!IsSupportedStereoBackbuffer(backBufferDescription)) {
        if (!m_loggedRejectedBackbuffer) {
            Log("Ignoring non-stereo backbuffer %ux%u (format=%d, samples=%u); expected %ux%u from CryEngine's live eye textures. Check that the root KCD1VR.cfg was loaded.",
                backBufferDescription.Width, backBufferDescription.Height,
                static_cast<int>(backBufferDescription.Format),
                backBufferDescription.SampleDesc.Count,
                m_engineEyeWidth * 2, m_engineEyeHeight);
            m_loggedRejectedBackbuffer = true;
        }
        return false;
    }
    const std::uint32_t sourceWidth = backBufferDescription.Width / 2;
    const std::uint32_t sourceHeight = backBufferDescription.Height;
    if (sourceWidth == 0 || sourceHeight == 0 || backBufferDescription.Width % 2 != 0 ||
        backBufferDescription.SampleDesc.Count != 1) {
        Log("Backbuffer must be even-width, non-MSAA side-by-side (got %ux%u, samples=%u)",
            backBufferDescription.Width, backBufferDescription.Height,
            backBufferDescription.SampleDesc.Count);
        return false;
    }

    std::uint32_t width = sourceWidth;
    std::uint32_t height = sourceHeight;
    DXGI_FORMAT sourceCopyFormat = backBufferDescription.Format;
    const bool dlssWasActive = m_dlssSwapchainActive;
    bool dlssReady = false;
    if (GetConfig().enableDlss && GetConfig().dlssOutputWidth > 0 &&
        GetConfig().dlssOutputHeight > 0) {
        const DXGI_FORMAT dlssFormat = DlssOutputFormat(backBufferDescription.Format);
        dlssReady = DlssUpscaler::Instance().Prepare(
            sourceWidth, sourceHeight, GetConfig().dlssOutputWidth,
            GetConfig().dlssOutputHeight, dlssFormat);
        if (GetConfig().dlssSubmitOutput && dlssReady &&
            (dlssWasActive || DlssUpscaler::Instance().InputsReady())) {
            width = GetConfig().dlssOutputWidth;
            height = GetConfig().dlssOutputHeight;
            sourceCopyFormat = dlssFormat;
        } else {
            dlssReady = false;
        }
    }

    std::uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr);
    std::vector<std::int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data());
    const DXGI_FORMAT swapchainFormat = SelectOpenXrColorFormat(
        sourceCopyFormat, formats, GetConfig().useOpenXrSrgbSwapchains);
    if (swapchainFormat == DXGI_FORMAT_UNKNOWN) {
        Log("OpenXR runtime supports neither the preferred color format nor source format %d",
            static_cast<int>(sourceCopyFormat));
        return false;
    }
    if (GetConfig().useOpenXrSrgbSwapchains &&
        swapchainFormat == sourceCopyFormat &&
        SrgbVariant(sourceCopyFormat) != sourceCopyFormat) {
        Log("OpenXR runtime does not support sRGB format %d; falling back to source format %d",
            static_cast<int>(SrgbVariant(sourceCopyFormat)),
            static_cast<int>(sourceCopyFormat));
    }

    if (m_stereoSwapchain != XR_NULL_HANDLE && !m_recreateSwapchain &&
        m_eyeWidth == width && m_eyeHeight == height &&
        m_swapchainFormat == swapchainFormat) {
        return true;
    }
    DestroySwapchain();

    const auto requested = static_cast<std::int64_t>(swapchainFormat);

    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.arraySize = 2;
    createInfo.format = requested;
    createInfo.width = width;
    createInfo.height = height;
    createInfo.mipCount = 1;
    createInfo.faceCount = 1;
    createInfo.sampleCount = 1;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    if (!Check(xrCreateSwapchain(m_session, &createInfo, &m_stereoSwapchain),
               "creating stereo swapchain")) {
        return false;
    }

    std::uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(m_stereoSwapchain, 0, &imageCount, nullptr);
    m_swapchainImages.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!Check(xrEnumerateSwapchainImages(
                   m_stereoSwapchain, imageCount, &imageCount,
                   reinterpret_cast<XrSwapchainImageBaseHeader*>(m_swapchainImages.data())),
               "enumerating stereo images")) {
        DestroySwapchain();
        return false;
    }

    m_eyeWidth = width;
    m_eyeHeight = height;
    m_swapchainFormat = swapchainFormat;
    m_dlssSwapchainActive = dlssReady;
    m_recreateSwapchain = false;
    m_loggedRejectedBackbuffer = false;
    Log("Stereo swapchain created: %ux%u per eye, source format=%d, OpenXR format=%d%s",
        width, height, static_cast<int>(sourceCopyFormat),
        static_cast<int>(m_swapchainFormat), dlssReady ? " (DLSS output)" : "");
    return true;
}

bool OpenXRRuntime::EnsureHudSwapchain(DXGI_FORMAT format)
{
    const std::uint32_t width = m_engineEyeWidth;
    const std::uint32_t height = m_engineEyeHeight;
    if (width == 0 || height == 0) {
        return false;
    }

    std::uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr);
    std::vector<std::int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data());
    const DXGI_FORMAT swapchainFormat = SelectOpenXrColorFormat(
        format, formats, GetConfig().useOpenXrSrgbSwapchains);
    if (swapchainFormat == DXGI_FORMAT_UNKNOWN) {
        Log("OpenXR runtime supports neither the preferred HUD color format nor source format %d",
            static_cast<int>(format));
        return false;
    }

    if (m_hudSwapchain != XR_NULL_HANDLE &&
        m_hudSwapchainFormat == swapchainFormat &&
        m_hudSwapchainWidth == width && m_hudSwapchainHeight == height) {
        return true;
    }
    DestroyHudSwapchain();

    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.arraySize = 1;
    createInfo.format = static_cast<std::int64_t>(swapchainFormat);
    createInfo.width = width;
    createInfo.height = height;
    createInfo.mipCount = 1;
    createInfo.faceCount = 1;
    createInfo.sampleCount = 1;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    if (!Check(xrCreateSwapchain(m_session, &createInfo, &m_hudSwapchain),
               "creating HUD swapchain")) {
        return false;
    }

    std::uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(m_hudSwapchain, 0, &imageCount, nullptr);
    m_hudSwapchainImages.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!Check(xrEnumerateSwapchainImages(
                   m_hudSwapchain, imageCount, &imageCount,
                   reinterpret_cast<XrSwapchainImageBaseHeader*>(
                       m_hudSwapchainImages.data())),
               "enumerating HUD images")) {
        DestroyHudSwapchain();
        return false;
    }
    m_hudSwapchainFormat = swapchainFormat;
    m_hudSwapchainWidth = width;
    m_hudSwapchainHeight = height;
    Log("HUD swapchain created: %ux%u, source format=%d, OpenXR format=%d",
        width, height, static_cast<int>(format),
        static_cast<int>(swapchainFormat));
    return true;
}

void OpenXRRuntime::DestroySwapchain()
{
    m_swapchainImages.clear();
    if (m_stereoSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_stereoSwapchain);
        m_stereoSwapchain = XR_NULL_HANDLE;
    }
    m_eyeWidth = 0;
    m_eyeHeight = 0;
    m_swapchainFormat = DXGI_FORMAT_UNKNOWN;
    m_dlssSwapchainActive = false;
    m_dlssHasValidFrame = false;
    m_loggedDlssFrameReuse = false;
}

void OpenXRRuntime::DestroyHudSwapchain()
{
    m_hudSwapchainImages.clear();
    if (m_hudSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_hudSwapchain);
        m_hudSwapchain = XR_NULL_HANDLE;
    }
    m_hudSwapchainFormat = DXGI_FORMAT_UNKNOWN;
    m_hudSwapchainWidth = 0;
    m_hudSwapchainHeight = 0;
}

void OpenXRRuntime::UpdateTrackingPose()
{
    const bool orientationValid =
        (m_views[0].pose.orientation.w != 0.0F || m_views[0].pose.orientation.x != 0.0F ||
         m_views[0].pose.orientation.y != 0.0F || m_views[0].pose.orientation.z != 0.0F);
    TrackingPose tracking;
    tracking.valid = orientationValid;
    tracking.pose.orientation = AverageOrientation(m_views[0].pose.orientation,
                                                   m_views[1].pose.orientation);
    tracking.pose.position = {
        (m_views[0].pose.position.x + m_views[1].pose.position.x) * 0.5F,
        (m_views[0].pose.position.y + m_views[1].pose.position.y) * 0.5F,
        (m_views[0].pose.position.z + m_views[1].pose.position.z) * 0.5F,
    };
    SetLatestTrackingPose(tracking);
    SetLatestEyeViews(m_views);
}

void OpenXRRuntime::PrepareNextFrame()
{
    if (!m_sessionRunning || m_framePrepared) {
        return;
    }

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (!Check(xrWaitFrame(m_session, &waitInfo, &frameState),
               "waiting for frame at render begin")) {
        return;
    }
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (!Check(xrBeginFrame(m_session, &beginInfo), "beginning frame")) {
        return;
    }

    m_framePrepared = true;
    m_shouldRender = frameState.shouldRender == XR_TRUE;
    m_displayTime = frameState.predictedDisplayTime;
    DlssUpscaler::Instance().BeginFrame();

    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = m_displayTime;
    locateInfo.space = m_space;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    std::uint32_t viewCount = 0;
    m_views = {{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    if (Check(xrLocateViews(m_session, &locateInfo, &viewState,
                            static_cast<std::uint32_t>(m_views.size()), &viewCount,
                            m_views.data()),
              "locating views") &&
        viewCount == 2 &&
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0) {
        UpdateTrackingPose();
    }
}

void OpenXRRuntime::SubmitPreparedFrame(ID3D11Texture2D* backBuffer,
                                        bool submitLayers)
{
    if (!m_framePrepared) {
        return;
    }

    const XrCompositionLayerBaseHeader* layers[2]{};
    std::uint32_t layerCount = 0;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView, 2> projectionViews{{
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
    }};
    XrCompositionLayerQuad hudLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};

    D3D11_TEXTURE2D_DESC backBufferDescription{};
    if (backBuffer) {
        backBuffer->GetDesc(&backBufferDescription);
    }
    const bool dialogueScreen =
        submitLayers && m_shouldRender && GetConfig().enableDialogueScreen &&
        m_dialogueScreenThisFrame && IsSupportedStereoBackbuffer(backBufferDescription);

    if (submitLayers && m_shouldRender && !dialogueScreen &&
        EnsureSwapchain(backBuffer)) {
        std::uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        imageWait.timeout = XR_INFINITE_DURATION;
        if (Check(xrAcquireSwapchainImage(m_stereoSwapchain, &acquireInfo, &imageIndex),
                  "acquiring stereo image") &&
            Check(xrWaitSwapchainImage(m_stereoSwapchain, &imageWait),
                  "waiting for stereo image")) {
            ID3D11Texture2D* destination = m_swapchainImages[imageIndex].texture;
            if (GetConfig().enableDlss && !GetConfig().dlssSubmitOutput &&
                DlssUpscaler::Instance().InputsReady()) {
                const bool leftProbe = DlssUpscaler::Instance().UpscaleEye(0);
                const bool rightProbe = DlssUpscaler::Instance().UpscaleEye(1);
                if (leftProbe && rightProbe && !m_loggedDlssOffscreenProbe) {
                    Log("DLSS: off-screen stereo probe completed; native VR submission remains active");
                    m_loggedDlssOffscreenProbe = true;
                }
            }
            const bool useDlss = m_dlssSwapchainActive;
            bool dlssEvaluated = useDlss;
            if (useDlss) {
                for (std::uint32_t eye = 0; eye < 2; ++eye) {
                    if (!DlssUpscaler::Instance().UpscaleEye(eye)) {
                        dlssEvaluated = false;
                        break;
                    }
                }
                if (dlssEvaluated) {
                    m_dlssHasValidFrame = true;
                    m_loggedDlssFrameReuse = false;
                } else if (m_dlssHasValidFrame && !m_loggedDlssFrameReuse) {
                    Log("DLSS: reusing the last complete stereo output because this frame missed an eye input");
                    m_loggedDlssFrameReuse = true;
                }
            }
            const bool haveDlssFrame =
                useDlss && (dlssEvaluated || m_dlssHasValidFrame);
            if (haveDlssFrame) {
                DlssUpscaler::Instance().BeginOutputCopyTiming();
                for (std::uint32_t eye = 0; eye < 2; ++eye) {
                    m_context->CopySubresourceRegion(
                        destination, D3D11CalcSubresource(0, eye, 1), 0, 0, 0,
                        DlssUpscaler::Instance().OutputTexture(eye), 0, nullptr);
                }
                DlssUpscaler::Instance().EndOutputCopyTiming();
            } else if (!useDlss && m_hudSeparatedThisFrame && m_worldEyeTextures[0] &&
                       m_worldEyeTextures[1]) {
                for (std::uint32_t eye = 0; eye < 2; ++eye) {
                    m_context->CopySubresourceRegion(
                        destination, D3D11CalcSubresource(0, eye, 1), 0, 0, 0,
                        m_worldEyeTextures[eye].Get(), 0, nullptr);
                }
            } else if (!useDlss) {
                const D3D11_BOX leftBox{0, 0, 0, m_eyeWidth, m_eyeHeight, 1};
                const D3D11_BOX rightBox{m_eyeWidth, 0, 0, m_eyeWidth * 2,
                                         m_eyeHeight, 1};
                m_context->CopySubresourceRegion(
                    destination, D3D11CalcSubresource(0, 0, 1), 0, 0, 0,
                    backBuffer, 0, &leftBox);
                m_context->CopySubresourceRegion(
                    destination, D3D11CalcSubresource(0, 1, 1), 0, 0, 0,
                    backBuffer, 0, &rightBox);
            }
            if (!useDlss || haveDlssFrame) {
                m_context->Flush();
            }
            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            const bool released = Check(
                xrReleaseSwapchainImage(m_stereoSwapchain, &releaseInfo),
                "releasing stereo image");
            if ((!useDlss || haveDlssFrame) && released) {
                constexpr float pi = 3.14159265358979323846F;
                const float vertical = GetConfig().renderFovDegrees * pi / 180.0F;
                const float aspect = static_cast<float>(m_eyeWidth) /
                                     static_cast<float>(m_eyeHeight);
                const float horizontal = 2.0F * std::atan(std::tan(vertical * 0.5F) * aspect);
                const XrFovf renderedFov{-horizontal * 0.5F, horizontal * 0.5F,
                                         vertical * 0.5F, -vertical * 0.5F};
                for (std::uint32_t eye = 0; eye < 2; ++eye) {
                    projectionViews[eye].pose = m_views[eye].pose;
                    projectionViews[eye].fov =
                        m_useOpenXrProjection ? m_views[eye].fov : renderedFov;
                    projectionViews[eye].subImage.swapchain = m_stereoSwapchain;
                    projectionViews[eye].subImage.imageRect.extent = {
                        static_cast<std::int32_t>(m_eyeWidth),
                        static_cast<std::int32_t>(m_eyeHeight)};
                    projectionViews[eye].subImage.imageArrayIndex = eye;
                }
                layer.space = m_space;
                layer.viewCount = 2;
                layer.views = projectionViews.data();
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
                layerCount = 1;
            }
        }
    }

    if (submitLayers && m_shouldRender &&
        (dialogueScreen || m_hudSeparatedThisFrame) &&
        EnsureHudSwapchain(m_engineEyeFormat)) {
        std::uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        imageWait.timeout = XR_INFINITE_DURATION;
        if (Check(xrAcquireSwapchainImage(m_hudSwapchain, &acquireInfo, &imageIndex),
                  "acquiring HUD image") &&
            Check(xrWaitSwapchainImage(m_hudSwapchain, &imageWait),
                  "waiting for HUD image")) {
            const std::uint32_t hudWidth = m_hudSwapchainWidth;
            const std::uint32_t hudHeight = m_hudSwapchainHeight;
            ID3D11Texture2D* hudDestination =
                m_hudSwapchainImages[imageIndex].texture;
            // 0.1.64 HUD behavior: capture only the left eye rectangle from
            // KCD's final side-by-side buffer. During dialogue that rectangle
            // contains the complete world and UI and becomes an opaque screen.
            const D3D11_BOX hudBox{0, 0, 0, hudWidth, hudHeight, 1};
            m_context->CopySubresourceRegion(hudDestination, 0, 0, 0, 0,
                                             backBuffer, 0, &hudBox);
            m_context->Flush();
            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if (Check(xrReleaseSwapchainImage(m_hudSwapchain, &releaseInfo),
                      "releasing HUD image")) {
                hudLayer.layerFlags = dialogueScreen
                                          ? 0
                                          : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                hudLayer.space = m_viewSpace;
                hudLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                hudLayer.pose.orientation.w = 1.0F;
                hudLayer.pose.position.z =
                    -(dialogueScreen ? GetConfig().dialogueScreenDistance
                                     : GetConfig().hudDistance);
                hudLayer.size.width =
                    dialogueScreen ? GetConfig().dialogueScreenWidth
                                   : GetConfig().hudWidth;
                const float dialogueAspect =
                    GetConfig().dialogueScreenAspect > 0.0F
                        ? GetConfig().dialogueScreenAspect
                        : static_cast<float>(hudWidth) /
                              static_cast<float>(hudHeight);
                hudLayer.size.height = dialogueScreen
                                           ? hudLayer.size.width / dialogueAspect
                                           : hudLayer.size.width *
                                                 static_cast<float>(hudHeight) /
                                                 static_cast<float>(hudWidth);
                hudLayer.subImage.swapchain = m_hudSwapchain;
                hudLayer.subImage.imageRect.extent = {
                    static_cast<std::int32_t>(hudWidth),
                    static_cast<std::int32_t>(hudHeight)};
                layers[layerCount++] =
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudLayer);
                if (dialogueScreen && !m_loggedDialogueScreen) {
                    Log("Dialogue screen submitted: source=%ux%u, %.2fm x %.2fm at %.2fm",
                        hudWidth, hudHeight, hudLayer.size.width,
                        hudLayer.size.height, GetConfig().dialogueScreenDistance);
                    m_loggedDialogueScreen = true;
                }
            }
        }
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = m_displayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount ? layers : nullptr;
    Check(xrEndFrame(m_session, &endInfo), "ending frame");
    if (submitLayers && m_hudSeparatedThisFrame && m_worldEyeTextures[0] &&
        m_worldEyeTextures[1]) {
        // Rebuild a useful side-by-side world mirror after the transparent HUD
        // texture has been submitted to OpenXR.
        m_context->CopySubresourceRegion(backBuffer, 0, 0, 0, 0,
                                         m_worldEyeTextures[0].Get(), 0, nullptr);
        m_context->CopySubresourceRegion(backBuffer, 0, m_engineEyeWidth, 0, 0,
                                         m_worldEyeTextures[1].Get(), 0, nullptr);
    }
    m_hudSeparatedThisFrame = false;
    m_dialogueScreenThisFrame = false;
    DlssUpscaler::Instance().EndFrameGpuTiming();
    m_framePrepared = false;
}

void OpenXRRuntime::SetOpenXRProjectionEnabled(bool enabled)
{
    m_useOpenXrProjection = enabled;
    Log("OpenXR headset projection %s", enabled ? "enabled" : "disabled");
}

void OpenXRRuntime::SetHudSeparationEnabled(bool enabled)
{
    m_hudSeparationEnabled = enabled;
    Log("OpenXR HUD quad %s", enabled ? "enabled" : "disabled");
}

void OpenXRRuntime::OnHudRenderBegin(ID3D11Texture2D* leftEye,
                                     ID3D11Texture2D* rightEye)
{
    std::scoped_lock lock(m_mutex);
    if (!m_device || !leftEye || !rightEye) {
        return;
    }

    std::array<ID3D11Texture2D*, 2> engineEyes{leftEye, rightEye};
    std::array<D3D11_TEXTURE2D_DESC, 2> descriptions{};
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        engineEyes[eye]->GetDesc(&descriptions[eye]);
        Microsoft::WRL::ComPtr<ID3D11Device> sourceDevice;
        engineEyes[eye]->GetDevice(&sourceDevice);
        if (sourceDevice.Get() != m_device.Get() ||
            descriptions[eye].Width == 0 || descriptions[eye].Height == 0 ||
            descriptions[eye].SampleDesc.Count != 1 ||
            descriptions[eye].ArraySize != 1 ||
            descriptions[eye].MipLevels != 1 ||
            (descriptions[eye].BindFlags & D3D11_BIND_RENDER_TARGET) == 0) {
            if (!m_loggedRejectedHudEyes) {
                Log("HUD separation rejected eye %u texture: %ux%u, format=%d, mips=%u, array=%u, samples=%u, bind=0x%X",
                    eye, descriptions[eye].Width, descriptions[eye].Height,
                    static_cast<int>(descriptions[eye].Format),
                    descriptions[eye].MipLevels, descriptions[eye].ArraySize,
                    descriptions[eye].SampleDesc.Count, descriptions[eye].BindFlags);
                m_loggedRejectedHudEyes = true;
            }
            return;
        }
        if (eye == 1 &&
            (descriptions[1].Width != descriptions[0].Width ||
             descriptions[1].Height != descriptions[0].Height ||
             descriptions[1].Format != descriptions[0].Format)) {
            if (!m_loggedRejectedHudEyes) {
                Log("Rejected mismatched stereo eye textures: left=%ux%u format=%d, right=%ux%u format=%d",
                    descriptions[0].Width, descriptions[0].Height,
                    static_cast<int>(descriptions[0].Format),
                    descriptions[1].Width, descriptions[1].Height,
                    static_cast<int>(descriptions[1].Format));
                m_loggedRejectedHudEyes = true;
            }
            return;
        }
    }

    const bool dimensionsChanged =
        m_engineEyeWidth != descriptions[0].Width ||
        m_engineEyeHeight != descriptions[0].Height ||
        m_engineEyeFormat != descriptions[0].Format;
    if (dimensionsChanged) {
        m_engineEyeWidth = descriptions[0].Width;
        m_engineEyeHeight = descriptions[0].Height;
        m_engineEyeFormat = descriptions[0].Format;
        m_recreateSwapchain = true;
        m_loggedRejectedBackbuffer = false;
        Log("Observed CryEngine stereo eye textures: %ux%u per eye, format=%d",
            m_engineEyeWidth, m_engineEyeHeight,
            static_cast<int>(m_engineEyeFormat));
    }
    m_loggedRejectedHudEyes = false;

    // PostAA executes on CryEngine's render thread, while its camera hook may
    // run on another thread. Supplying the real eye targets lets the DLSS
    // capture path resolve the eye directly from the currently bound RTV.
    SetDlssEyeRenderTargets(engineEyes[0], engineEyes[1]);

    // Keep direct references to the two validated engine eyes. The dialogue
    // screen uses the final backbuffer, but retaining these here preserves the
    // exact 0.1.64 resource-observation and resize behavior.
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        if (dimensionsChanged || m_engineEyeTextures[eye].Get() != engineEyes[eye]) {
            m_engineEyeRenderTargets[eye].Reset();
            m_engineEyeTextures[eye] = engineEyes[eye];
        }
    }

    if (GetConfig().enableDialogueScreen && IsDialogueCameraActive() &&
        GetConfig().openXrDiagnosticMode == 0 && m_sessionRunning &&
        m_framePrepared && m_context) {
        // Do not clear or separate anything. Scaleform will draw normally over
        // KCD's stereo world; Present will show the complete left eye as one
        // opaque, head-locked quad and omit the stereo projection layer.
        m_dialogueScreenThisFrame = true;
        m_hudSeparatedThisFrame = false;
        return;
    }

    // Eye observation remains active even when the HUD quad is disabled. It is
    // also the authoritative runtime stereo-size signal used by Present.
    if (GetConfig().openXrDiagnosticMode != 0 || !m_hudSeparationEnabled ||
        !m_sessionRunning || !m_framePrepared || !m_context) {
        return;
    }

    if (!m_hudSeparatedThisFrame) {
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            if (dimensionsChanged ||
                m_engineEyeTextures[eye].Get() != engineEyes[eye]) {
                m_engineEyeRenderTargets[eye].Reset();
                m_engineEyeTextures[eye] = engineEyes[eye];
            }
            if (!m_engineEyeRenderTargets[eye] &&
                FAILED(m_device->CreateRenderTargetView(
                    engineEyes[eye], nullptr,
                    &m_engineEyeRenderTargets[eye]))) {
                Log("Unable to create cached eye %u render-target view", eye);
                return;
            }

            D3D11_TEXTURE2D_DESC captureDescription{};
            if (m_worldEyeTextures[eye]) {
                m_worldEyeTextures[eye]->GetDesc(&captureDescription);
            }
            if (!m_worldEyeTextures[eye] ||
                captureDescription.Width != descriptions[eye].Width ||
                captureDescription.Height != descriptions[eye].Height ||
                captureDescription.Format != descriptions[eye].Format) {
                m_worldEyeTextures[eye].Reset();
                captureDescription = descriptions[eye];
                captureDescription.BindFlags = 0;
                captureDescription.CPUAccessFlags = 0;
                captureDescription.MiscFlags = 0;
                captureDescription.Usage = D3D11_USAGE_DEFAULT;
                if (FAILED(m_device->CreateTexture2D(
                        &captureDescription, nullptr,
                        &m_worldEyeTextures[eye]))) {
                    Log("Unable to create eye %u world capture texture", eye);
                    return;
                }
            }
        }

        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            m_context->CopyResource(m_worldEyeTextures[eye].Get(), engineEyes[eye]);
        }
        constexpr float transparent[4]{0.0F, 0.0F, 0.0F, 0.0F};
        for (auto& renderTarget : m_engineEyeRenderTargets) {
            m_context->ClearRenderTargetView(renderTarget.Get(), transparent);
        }
        m_hudSeparatedThisFrame = true;
        if (!m_loggedHudCapture) {
            Log("Separated stereo world from Scaleform HUD in two %ux%u eye textures",
                m_engineEyeWidth, m_engineEyeHeight);
            m_loggedHudCapture = true;
        }
    }
}

void OpenXRRuntime::OnPresent(IDXGISwapChain* presentingSwapChain)
{
    std::scoped_lock lock(m_mutex);
    if (!presentingSwapChain || presentingSwapChain != m_swapChain.Get() || !m_session) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(presentingSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        return;
    }
    D3D11_TEXTURE2D_DESC description{};
    backBuffer->GetDesc(&description);
    if (description.Width != m_lastPresentWidth ||
        description.Height != m_lastPresentHeight) {
        Log("Main swapchain present size changed to %ux%u", description.Width,
            description.Height);
        m_lastPresentWidth = description.Width;
        m_lastPresentHeight = description.Height;
    }

    PollEvents();
    if (!m_sessionRunning) {
        return;
    }

    const int diagnosticMode = GetConfig().openXrDiagnosticMode;
    if (diagnosticMode == 2) {
        return;
    }

    SubmitPreparedFrame(backBuffer.Get(), diagnosticMode == 0);
    PrepareNextFrame();

    if ((GetAsyncKeyState(VK_F11) & 1) != 0) {
        RecenterTracking();
    }
}

void OpenXRRuntime::OnResize(IDXGISwapChain* resizedSwapChain)
{
    std::scoped_lock lock(m_mutex);
    if (resizedSwapChain == m_swapChain.Get()) {
        // The graphics menu can keep an OpenXR frame open while DXGI and
        // CryEngine rebuild several temporary-sized render targets. End that
        // frame now; carrying its old predicted display time and resources
        // across ResizeBuffers can leave the runtime permanently black.
        if (m_framePrepared) {
            XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = m_displayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            Check(xrEndFrame(m_session, &endInfo), "ending frame for DXGI resize");
            m_framePrepared = false;
        }
        DestroySwapchain();
        DestroyHudSwapchain();
        for (auto& texture : m_worldEyeTextures) {
            texture.Reset();
        }
        ResetObservedEyeTextures();
        m_recreateSwapchain = true;
        m_hudSeparatedThisFrame = false;
        m_loggedRejectedBackbuffer = false;
        m_loggedRejectedHudEyes = false;
        m_loggedHudCapture = false;
        Log("Released OpenXR frame resources for DXGI resize");
    }
}

void OpenXRRuntime::Shutdown()
{
    // Caller owns synchronization.
    if (m_sessionRunning) {
        if (m_framePrepared) {
            XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = m_displayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            xrEndFrame(m_session, &endInfo);
            m_framePrepared = false;
        }
        xrEndSession(m_session);
        m_sessionRunning = false;
    }
    DestroySwapchain();
    DestroyHudSwapchain();
    for (auto& texture : m_worldEyeTextures) {
        texture.Reset();
    }
    ResetObservedEyeTextures();
    if (m_viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(m_viewSpace);
        m_viewSpace = XR_NULL_HANDLE;
    }
    if (m_space != XR_NULL_HANDLE) {
        xrDestroySpace(m_space);
        m_space = XR_NULL_HANDLE;
    }
    if (m_session != XR_NULL_HANDLE) {
        xrDestroySession(m_session);
        m_session = XR_NULL_HANDLE;
    }
    if (m_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(m_instance);
        m_instance = XR_NULL_HANDLE;
    }
    DlssUpscaler::Instance().Shutdown();
    m_swapChain.Reset();
    m_lastPresentWidth = 0;
    m_lastPresentHeight = 0;
    m_context.Reset();
    m_device.Reset();
}

}  // namespace kcdvr
