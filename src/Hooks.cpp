#include "Hooks.h"

#include "Config.h"
#include "DlssUpscaler.h"
#include "KcdCamera.h"
#include "Log.h"
#include "OpenXRRuntime.h"
#include "ShaderCapture.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace kcdvr {
namespace {

using FactoryCreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);
using D3D11CreateDeviceFn = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
    UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using D3D11CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
    UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
    D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using PlayerUpdateViewFn = void(__fastcall*)(void*, SViewParamsPrefix*);
using StereoPrepareCameraFn = void*(__fastcall*)(void*, void*, int, const void*);
using CameraSetFrustumFn = void(__fastcall*)(void*, int, int, float, float, float, float);
using CameraUpdateFrustumFn = void(__fastcall*)(void*);
using FlashRenderInternalFn = void(__fastcall*)(void*, void*, bool, bool);
using DialogueCameraStateFn = void(__fastcall*)(void*);
using FlashAddRefFn = void(__fastcall*)(void*);
using FlashReleaseFn = void(__fastcall*)(void*);
using FlashSetViewportFn = void(__fastcall*)(void*, int, int, int, int, float);
using FlashGetViewportFn = void(__fastcall*)(void*, int*, int*, int*, int*, float*);

FactoryCreateSwapChainFn g_originalCreateSwapChain = nullptr;
PresentFn g_originalPresent = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;
D3D11CreateDeviceFn g_originalD3D11CreateDevice = nullptr;
D3D11CreateDeviceAndSwapChainFn g_originalD3D11CreateDeviceAndSwapChain = nullptr;
PlayerUpdateViewFn g_originalPlayerUpdateView = nullptr;
StereoPrepareCameraFn g_originalStereoPrepareCamera = nullptr;
CameraSetFrustumFn g_cameraSetFrustum = nullptr;
CameraUpdateFrustumFn g_cameraUpdateFrustum = nullptr;
FlashRenderInternalFn g_originalFlashRenderInternal = nullptr;
DialogueCameraStateFn g_originalDialogueCameraActivate = nullptr;
DialogueCameraStateFn g_originalDialogueCameraDeactivate = nullptr;
void* g_presentTarget = nullptr;
void* g_resizeTarget = nullptr;
void* g_cameraTarget = nullptr;
void* g_stereoCameraTarget = nullptr;
void* g_flashRenderTarget = nullptr;
void* g_dialogueCameraActivateTarget = nullptr;
void* g_dialogueCameraDeactivateTarget = nullptr;
bool g_loggedEyeTextureResolveFailure = false;
bool g_loggedDialogueViewportMismatch = false;
std::uint32_t g_loggedDialogueViewportChanges = 0;

constexpr std::size_t kCameraMatrixSize = 0x30;
constexpr std::size_t kCameraNearPlaneOffset = 0x54;
constexpr std::size_t kCameraFarPlaneOffset = 0x6C;
constexpr std::size_t kCameraAsymLeftOffset = 0x74;
constexpr std::size_t kCameraAsymRightOffset = 0x78;
constexpr std::size_t kCameraAsymBottomOffset = 0x7C;
constexpr std::size_t kCameraAsymTopOffset = 0x80;
constexpr std::size_t kCryTextureDevTextureOffset = 0x30;
constexpr std::size_t kFlashRenderProxyBaseOffset = 0x08;
constexpr std::size_t kFlashSetViewportSlot = 6;
constexpr std::size_t kFlashGetViewportSlot = 7;

struct FlashViewportOverride {
    void* player = nullptr;
    FlashReleaseFn release = nullptr;
    FlashSetViewportFn setViewport = nullptr;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    float aspect = 1.0F;
    bool referenceHeld = false;
    bool viewportChanged = false;
};

bool TryReadPointer(const void* address, void*& value)
{
    value = nullptr;
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || VirtualQuery(address, &memory, sizeof(memory)) == 0 ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        reinterpret_cast<std::uintptr_t>(address) + sizeof(void*) >
            reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize) {
        return false;
    }
    __try {
        value = *static_cast<void* const*>(address);
        return value != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = nullptr;
        return false;
    }
}

void ReleaseFlashViewportReference(FlashViewportOverride& state)
{
    if (!state.referenceHeld || !state.release || !state.player) {
        return;
    }
    __try {
        state.release(state.player);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Dialogue screen: exception while releasing Scaleform player reference");
    }
    state.referenceHeld = false;
}

bool BeginDialogueFlashViewport(void* renderProxy, std::uint32_t eyeWidth,
                                std::uint32_t eyeHeight,
                                FlashViewportOverride& state)
{
    if (!GetConfig().enableDialogueScreen || !IsDialogueCameraActive() ||
        !renderProxy || eyeWidth == 0 || eyeHeight == 0) {
        return false;
    }

    HMODULE game = GetModuleHandleW(L"WHGame.dll");
    if (!game) {
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    void* renderProxyVtable = nullptr;
    if (!TryReadPointer(renderProxy, renderProxyVtable) ||
        reinterpret_cast<std::uintptr_t>(renderProxyVtable) !=
            base + GetConfig().flashPlayerRenderProxyVtableRva) {
        if (!g_loggedDialogueViewportMismatch) {
            Log("Dialogue screen: Scaleform render-proxy vtable did not match; viewport override skipped");
            g_loggedDialogueViewportMismatch = true;
        }
        return false;
    }

    state.player = static_cast<std::uint8_t*>(renderProxy) -
                   kFlashRenderProxyBaseOffset;
    void* playerVtable = nullptr;
    if (!TryReadPointer(state.player, playerVtable) ||
        reinterpret_cast<std::uintptr_t>(playerVtable) !=
            base + GetConfig().flashPlayerVtableRva) {
        if (!g_loggedDialogueViewportMismatch) {
            Log("Dialogue screen: Scaleform player vtable did not match; viewport override skipped");
            g_loggedDialogueViewportMismatch = true;
        }
        state.player = nullptr;
        return false;
    }

    void* addRefAddress = nullptr;
    void* releaseAddress = nullptr;
    void* setViewportAddress = nullptr;
    void* getViewportAddress = nullptr;
    const auto* vtableBytes = static_cast<const std::uint8_t*>(playerVtable);
    if (!TryReadPointer(vtableBytes, addRefAddress) ||
        !TryReadPointer(vtableBytes + sizeof(void*), releaseAddress) ||
        !TryReadPointer(vtableBytes + kFlashSetViewportSlot * sizeof(void*),
                        setViewportAddress) ||
        !TryReadPointer(vtableBytes + kFlashGetViewportSlot * sizeof(void*),
                        getViewportAddress)) {
        state.player = nullptr;
        return false;
    }

    const auto addRef = reinterpret_cast<FlashAddRefFn>(addRefAddress);
    state.release = reinterpret_cast<FlashReleaseFn>(releaseAddress);
    state.setViewport = reinterpret_cast<FlashSetViewportFn>(setViewportAddress);
    const auto getViewport =
        reinterpret_cast<FlashGetViewportFn>(getViewportAddress);
    __try {
        addRef(state.player);
        state.referenceHeld = true;
        getViewport(state.player, &state.x, &state.y, &state.width,
                    &state.height, &state.aspect);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Dialogue screen: exception while reading Scaleform viewport");
        ReleaseFlashViewportReference(state);
        return false;
    }

    if (state.width <= 0 || state.height <= 0 || state.width > 32768 ||
        state.height > 32768 || !std::isfinite(state.aspect)) {
        ReleaseFlashViewportReference(state);
        return false;
    }

    // KCD sizes its stereo Scaleform canvas for the complete side-by-side
    // output. A flat dialogue screen contains one eye, so give the movie that
    // eye's real viewport while it draws. Restore it immediately afterward.
    if (state.x != 0 || state.y != 0 ||
        state.width != static_cast<int>(eyeWidth) ||
        state.height != static_cast<int>(eyeHeight)) {
        __try {
            state.setViewport(state.player, 0, 0, static_cast<int>(eyeWidth),
                              static_cast<int>(eyeHeight), state.aspect);
            state.viewportChanged = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("Dialogue screen: exception while setting Scaleform viewport");
            ReleaseFlashViewportReference(state);
            return false;
        }
        if (g_loggedDialogueViewportChanges < 8) {
            Log("Dialogue screen: Scaleform viewport %d,%d %dx%d -> 0,0 %ux%u (aspect %.3f)",
                state.x, state.y, state.width, state.height, eyeWidth,
                eyeHeight, state.aspect);
            ++g_loggedDialogueViewportChanges;
        }
    }
    return true;
}

void EndDialogueFlashViewport(FlashViewportOverride& state)
{
    if (state.viewportChanged && state.setViewport && state.player) {
        __try {
            state.setViewport(state.player, state.x, state.y, state.width,
                              state.height, state.aspect);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("Dialogue screen: exception while restoring Scaleform viewport");
        }
    }
    ReleaseFlashViewportReference(state);
}

bool TryQueryTexture2D(void* resource, ID3D11Texture2D** texture)
{
    *texture = nullptr;
    void* vtable = nullptr;
    if (!TryReadPointer(resource, vtable)) {
        return false;
    }
    __try {
        return SUCCEEDED(static_cast<IUnknown*>(resource)->QueryInterface(
            __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *texture = nullptr;
        return false;
    }
}

bool ResolveCryTexture2D(std::uintptr_t globalAddress,
                         Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture)
{
    texture.Reset();
    void* cryTexture = nullptr;
    void* deviceTexture = nullptr;
    void* resource = nullptr;
    if (!TryReadPointer(reinterpret_cast<void*>(globalAddress), cryTexture) ||
        !TryReadPointer(static_cast<std::uint8_t*>(cryTexture) +
                            kCryTextureDevTextureOffset,
                        deviceTexture) ||
        !TryReadPointer(deviceTexture, resource)) {
        return false;
    }
    ID3D11Texture2D* resolved = nullptr;
    if (!TryQueryTexture2D(resource, &resolved)) {
        return false;
    }
    texture.Attach(resolved);
    return true;
}

bool HookFunction(void* target, void* replacement, void** original, const char* name)
{
    const MH_STATUS createStatus = MH_CreateHook(target, replacement, original);
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
        Log("MinHook failed to create %s hook: %s", name, MH_StatusToString(createStatus));
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
        Log("MinHook failed to enable %s hook: %s", name, MH_StatusToString(enableStatus));
        return false;
    }
    Log("Installed %s hook at %p", name, target);
    return true;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    OnShaderBrowserPresent();
    OpenXRRuntime::Instance().OnPresent(swapChain);
    return g_originalPresent(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount,
                                             UINT width, UINT height, DXGI_FORMAT format,
                                             UINT flags)
{
    Log("DXGI ResizeBuffers: swapchain=%p, buffers=%u, requested=%ux%u, format=%d",
        swapChain, bufferCount, width, height, static_cast<int>(format));
    OpenXRRuntime::Instance().OnResize(swapChain);
    return g_originalResizeBuffers(swapChain, bufferCount, width, height, format, flags);
}

void HookSwapChain(IDXGISwapChain* swapChain, ID3D11Device* device)
{
    if (!swapChain || !device) {
        return;
    }
    void** vtable = *reinterpret_cast<void***>(swapChain);
    if (!g_originalPresent) {
        g_presentTarget = vtable[8];
        HookFunction(g_presentTarget, reinterpret_cast<void*>(&HookPresent),
                     reinterpret_cast<void**>(&g_originalPresent), "IDXGISwapChain::Present");
    }
    if (!g_originalResizeBuffers) {
        g_resizeTarget = vtable[13];
        HookFunction(g_resizeTarget, reinterpret_cast<void*>(&HookResizeBuffers),
                     reinterpret_cast<void**>(&g_originalResizeBuffers),
                     "IDXGISwapChain::ResizeBuffers");
    }
    InstallShaderCaptureHook(device);
    OpenXRRuntime::Instance().Attach(device, swapChain);
}

HRESULT WINAPI HookD3D11CreateDevice(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount,
    UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel,
    ID3D11DeviceContext** immediateContext)
{
    const HRESULT result = g_originalD3D11CreateDevice(
        adapter, driverType, software, flags, featureLevels, featureLevelCount,
        sdkVersion, device, featureLevel, immediateContext);
    if (SUCCEEDED(result) && device && *device) {
        InstallShaderCaptureHook(*device);
    }
    return result;
}

HRESULT WINAPI HookD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount,
    UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* swapChainDescription,
    IDXGISwapChain** swapChain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** immediateContext)
{
    const HRESULT result = g_originalD3D11CreateDeviceAndSwapChain(
        adapter, driverType, software, flags, featureLevels, featureLevelCount,
        sdkVersion, swapChainDescription, swapChain, device, featureLevel,
        immediateContext);
    if (SUCCEEDED(result) && device && *device) {
        InstallShaderCaptureHook(*device);
        if (swapChain && *swapChain) {
            HookSwapChain(*swapChain, *device);
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* factory, IUnknown* deviceUnknown,
                                               DXGI_SWAP_CHAIN_DESC* description,
                                               IDXGISwapChain** swapChain)
{
    const HRESULT result =
        g_originalCreateSwapChain(factory, deviceUnknown, description, swapChain);
    if (SUCCEEDED(result) && swapChain && *swapChain) {
        Log("DXGI swapchain created: swapchain=%p, requested=%ux%u, format=%d, hwnd=%p",
            *swapChain,
            description ? description->BufferDesc.Width : 0,
            description ? description->BufferDesc.Height : 0,
            description ? static_cast<int>(description->BufferDesc.Format) : 0,
            description ? description->OutputWindow : nullptr);
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        if (SUCCEEDED(deviceUnknown->QueryInterface(IID_PPV_ARGS(&device)))) {
            HookSwapChain(*swapChain, device.Get());
        }
    }
    return result;
}

void __fastcall HookPlayerUpdateView(void* self, SViewParamsPrefix* view)
{
    g_originalPlayerUpdateView(self, view);
    if (view && GetConfig().cullingFovDegrees > 0.0F) {
        constexpr float pi = 3.14159265358979323846F;
        const float cullingFov = GetConfig().cullingFovDegrees * pi / 180.0F;
        if (view->fov < cullingFov) {
            // This center-camera FOV is consumed by CryEngine visibility/LOD
            // work. PrepareCamera later replaces each rendered eye with the
            // exact OpenXR projection, so headset scale and alignment remain
            // unchanged.
            view->fov = cullingFov;
        }
    }
    if (view && GetConfig().lockVerticalCameraInput) {
        LockGameCameraToHorizontal(*view);
    }
    if (view && GetConfig().enableHeadTracking) {
        ApplyTrackingPose(*view);
    }
}

float ReadCameraFloat(const void* camera, std::size_t offset)
{
    float value = 0.0F;
    std::memcpy(&value, static_cast<const std::uint8_t*>(camera) + offset,
                sizeof(value));
    return value;
}

void WriteCameraFloat(void* camera, std::size_t offset, float value)
{
    std::memcpy(static_cast<std::uint8_t*>(camera) + offset, &value,
                sizeof(value));
}

bool ApplyOpenXREyeCamera(void* result, const void* centerCamera, int eye)
{
    if (!result || !centerCamera || eye < 0 || eye > 1 || !g_cameraSetFrustum ||
        !g_cameraUpdateFrustum) {
        return false;
    }

    const EyeCameraGeometry geometry =
        GetLatestEyeGeometry(static_cast<std::size_t>(eye));
    if (!geometry.valid) {
        return false;
    }

    const float tangentLeft = std::tan(geometry.fov.angleLeft);
    const float tangentRight = std::tan(geometry.fov.angleRight);
    const float tangentUp = std::tan(geometry.fov.angleUp);
    const float tangentDown = std::tan(geometry.fov.angleDown);
    const float halfWidth = (tangentRight - tangentLeft) * 0.5F;
    const float halfHeight = (tangentUp - tangentDown) * 0.5F;
    if (!(halfWidth > 0.0001F) || !(halfHeight > 0.0001F)) {
        return false;
    }

    // Discard CryEngine's monitor-oriented stereo shift and start from the
    // centered camera. Matrix34 is row-major, with translation in column 3.
    std::memcpy(result, centerCamera, kCameraMatrixSize);
    float* matrix = static_cast<float*>(result);
    const float scale = GetConfig().worldScale;
    const float localX = geometry.offset.x * scale;
    const float localY = geometry.offset.y * scale;
    const float localZ = geometry.offset.z * scale;
    matrix[3] += matrix[0] * localX + matrix[1] * localY + matrix[2] * localZ;
    matrix[7] += matrix[4] * localX + matrix[5] * localY + matrix[6] * localZ;
    matrix[11] += matrix[8] * localX + matrix[9] * localY + matrix[10] * localZ;

    const float nearPlane = ReadCameraFloat(result, kCameraNearPlaneOffset);
    const float farPlane = ReadCameraFloat(result, kCameraFarPlaneOffset);
    if (!(nearPlane > 0.001F) || !(farPlane > nearPlane)) {
        return false;
    }
    SetDlssEyeClipPlanes(eye, nearPlane, farPlane);

    const float verticalFov = 2.0F * std::atan(halfHeight);
    const float aspectRatio = halfWidth / halfHeight;
    g_cameraSetFrustum(result, 1, 1, verticalFov, nearPlane, farPlane,
                       1.0F / aspectRatio);

    float asymmetryHorizontal =
        (tangentRight + tangentLeft) * 0.5F * nearPlane;
    float asymmetryVertical =
        (tangentUp + tangentDown) * 0.5F * nearPlane;
    if (GetConfig().enableDlss) {
        const DlssJitter jitter = DlssUpscaler::Instance().CurrentJitter();
        if (jitter.renderWidth > 0 && jitter.renderHeight > 0) {
            const float frustumWidth = 2.0F * halfWidth * nearPlane;
            const float frustumHeight = 2.0F * halfHeight * nearPlane;
            asymmetryHorizontal -=
                jitter.x * frustumWidth /
                static_cast<float>(jitter.renderWidth);
            asymmetryVertical +=
                jitter.y * frustumHeight /
                static_cast<float>(jitter.renderHeight);
        }
    }
    WriteCameraFloat(result, kCameraAsymLeftOffset, asymmetryHorizontal);
    WriteCameraFloat(result, kCameraAsymRightOffset, asymmetryHorizontal);
    WriteCameraFloat(result, kCameraAsymBottomOffset, asymmetryVertical);
    WriteCameraFloat(result, kCameraAsymTopOffset, asymmetryVertical);
    g_cameraUpdateFrustum(result);
    return true;
}

void __fastcall HookDialogueCameraActivate(void* self)
{
    g_originalDialogueCameraActivate(self);
    const bool wasActive = IsDialogueCameraActive();
    SetDialogueCameraActive(true);
    if (!wasActive) {
        Log(GetConfig().enableDialogueScreen
                ? "Dialogue camera active; flat dialogue screen armed"
                : "Dialogue camera active; flat dialogue screen disabled");
    }
}

void __fastcall HookDialogueCameraDeactivate(void* self)
{
    g_originalDialogueCameraDeactivate(self);
    const bool wasActive = IsDialogueCameraActive();
    SetDialogueCameraActive(false);
    if (wasActive) {
        Log("Dialogue camera inactive; normal 0.1.64 projection and HUD remain active");
    }
}

void* __fastcall HookStereoPrepareCamera(void* self, void* result, int eye,
                                         const void* centerCamera)
{
    void* returned = g_originalStereoPrepareCamera(self, result, eye, centerCamera);
    if (GetConfig().enableOpenXRProjection) {
        ApplyOpenXREyeCamera(result, centerCamera, eye);
    }
    return returned;
}

void __fastcall HookFlashRenderInternal(void* self, void* player, bool stereo,
                                        bool doRealRender)
{
    FlashViewportOverride viewportOverride{};
    if (doRealRender) {
        HMODULE game = GetModuleHandleW(L"WHGame.dll");
        if (game) {
            const auto base = reinterpret_cast<std::uintptr_t>(game);
            Microsoft::WRL::ComPtr<ID3D11Texture2D> leftEye;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> rightEye;
            if (ResolveCryTexture2D(base + GetConfig().stereoLeftTextureRva, leftEye) &&
                ResolveCryTexture2D(base + GetConfig().stereoRightTextureRva, rightEye)) {
                D3D11_TEXTURE2D_DESC eyeDescription{};
                leftEye->GetDesc(&eyeDescription);
                BeginDialogueFlashViewport(player, eyeDescription.Width,
                                            eyeDescription.Height,
                                            viewportOverride);
                OpenXRRuntime::Instance().OnHudRenderBegin(leftEye.Get(),
                                                           rightEye.Get());
            } else if (!g_loggedEyeTextureResolveFailure) {
                Log("Could not resolve KCD's private stereo eye textures; HUD separation is inactive");
                g_loggedEyeTextureResolveFailure = true;
            }
        }
    }
    g_originalFlashRenderInternal(self, player, stereo, doRealRender);
    EndDialogueFlashViewport(viewportOverride);
}

bool InstallDxgiHook()
{
    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    if (FAILED(CreateDXGIFactory(IID_PPV_ARGS(&factory)))) {
        Log("CreateDXGIFactory failed while installing hook");
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(factory.Get());
    return HookFunction(vtable[10], reinterpret_cast<void*>(&HookCreateSwapChain),
                        reinterpret_cast<void**>(&g_originalCreateSwapChain),
                        "IDXGIFactory::CreateSwapChain");
}

bool InstallD3D11DeviceHooks()
{
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) {
        // Load the Windows system runtime ourselves so its exported creation
        // entry points can be intercepted before CryEngine asks for a device.
        d3d11 = LoadLibraryW(L"d3d11.dll");
    }
    if (!d3d11) {
        Log("Could not load d3d11.dll while installing early device hooks");
        return false;
    }
    void* createDevice = reinterpret_cast<void*>(
        GetProcAddress(d3d11, "D3D11CreateDevice"));
    void* createDeviceAndSwapChain = reinterpret_cast<void*>(
        GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (!createDevice || !createDeviceAndSwapChain) {
        Log("Could not resolve the D3D11 device creation exports");
        return false;
    }
    bool installed = HookFunction(
        createDevice, reinterpret_cast<void*>(&HookD3D11CreateDevice),
        reinterpret_cast<void**>(&g_originalD3D11CreateDevice),
        "D3D11CreateDevice");
    installed &= HookFunction(
        createDeviceAndSwapChain,
        reinterpret_cast<void*>(&HookD3D11CreateDeviceAndSwapChain),
        reinterpret_cast<void**>(&g_originalD3D11CreateDeviceAndSwapChain),
        "D3D11CreateDeviceAndSwapChain");
    return installed;
}

bool InstallCameraHook()
{
    HMODULE game = GetModuleHandleW(L"WHGame.dll");
    if (!game) {
        Log("WHGame.dll was not loaded; camera hook unavailable");
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto vtableAddress = base + GetConfig().playerViewVtableRva;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<void*>(vtableAddress), &memory, sizeof(memory)) == 0 ||
        memory.AllocationBase != game) {
        Log("Configured player view vtable RVA is outside WHGame.dll");
        return false;
    }

    void** vtable = reinterpret_cast<void**>(vtableAddress);
    g_cameraTarget = vtable[1];
    const auto target = reinterpret_cast<std::uintptr_t>(g_cameraTarget);
    if (target < base || target >= base + 0x4000000ULL) {
        Log("Player camera target %p failed module-range validation", g_cameraTarget);
        return false;
    }

    constexpr std::array<std::uint8_t, 12> expected{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48};
    if (std::memcmp(g_cameraTarget, expected.data(), expected.size()) != 0) {
        Log("Player camera signature mismatch at %p; refusing unsafe hook", g_cameraTarget);
        return false;
    }

    return HookFunction(g_cameraTarget, reinterpret_cast<void*>(&HookPlayerUpdateView),
                        reinterpret_cast<void**>(&g_originalPlayerUpdateView),
                        "C_Player::IGameObjectView::UpdateView");
}

bool ValidateModuleAddress(HMODULE module, std::uintptr_t address, const char* name)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) == 0 ||
        memory.AllocationBase != module) {
        Log("Configured %s RVA is outside WHGame.dll", name);
        return false;
    }
    return true;
}

bool InstallStereoCameraHook()
{
    if (!GetConfig().enableOpenXRProjection) {
        Log("OpenXR per-eye projection is disabled; using CryEngine stereo camera");
        return true;
    }

    HMODULE game = GetModuleHandleW(L"WHGame.dll");
    if (!game) {
        Log("WHGame.dll was not loaded; stereo camera hook unavailable");
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto prepareAddress = base + GetConfig().stereoPrepareCameraRva;
    const auto setFrustumAddress = base + GetConfig().cameraSetFrustumRva;
    const auto updateFrustumAddress = base + GetConfig().cameraUpdateFrustumRva;
    if (!ValidateModuleAddress(game, prepareAddress, "stereo PrepareCamera") ||
        !ValidateModuleAddress(game, setFrustumAddress, "camera SetFrustum") ||
        !ValidateModuleAddress(game, updateFrustumAddress, "camera UpdateFrustum")) {
        return false;
    }

    constexpr std::array<std::uint8_t, 15> prepareSignature{
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x57,
        0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 12> setFrustumSignature{
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
        0x89, 0x70, 0x10, 0x57};
    constexpr std::array<std::uint8_t, 15> updateFrustumSignature{
        0x48, 0x8B, 0xC4, 0x55, 0x48, 0x8D, 0x68, 0xA1,
        0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00};
    if (std::memcmp(reinterpret_cast<void*>(prepareAddress), prepareSignature.data(),
                    prepareSignature.size()) != 0 ||
        std::memcmp(reinterpret_cast<void*>(setFrustumAddress), setFrustumSignature.data(),
                    setFrustumSignature.size()) != 0 ||
        std::memcmp(reinterpret_cast<void*>(updateFrustumAddress),
                    updateFrustumSignature.data(), updateFrustumSignature.size()) != 0) {
        Log("Stereo camera helper signature mismatch; keeping working fallback projection");
        return false;
    }

    g_stereoCameraTarget = reinterpret_cast<void*>(prepareAddress);
    g_cameraSetFrustum = reinterpret_cast<CameraSetFrustumFn>(setFrustumAddress);
    g_cameraUpdateFrustum = reinterpret_cast<CameraUpdateFrustumFn>(updateFrustumAddress);
    return HookFunction(g_stereoCameraTarget,
                        reinterpret_cast<void*>(&HookStereoPrepareCamera),
                        reinterpret_cast<void**>(&g_originalStereoPrepareCamera),
                        "CD3DStereoRenderer::PrepareCamera");
}

bool InstallHudRenderHook()
{
    HMODULE game = GetModuleHandleW(L"WHGame.dll");
    if (!game) {
        Log("WHGame.dll was not loaded; stereo eye observer unavailable");
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(game) +
                         GetConfig().flashRenderRva;
    if (!ValidateModuleAddress(game, address, "FlashRenderInternal")) {
        return false;
    }
    constexpr std::array<std::uint8_t, 15> signature{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20};
    if (std::memcmp(reinterpret_cast<void*>(address), signature.data(),
                    signature.size()) != 0) {
        Log("Flash renderer signature mismatch; stereo eye observer unavailable");
        return false;
    }

    g_flashRenderTarget = reinterpret_cast<void*>(address);
    return HookFunction(g_flashRenderTarget,
                        reinterpret_cast<void*>(&HookFlashRenderInternal),
                        reinterpret_cast<void**>(&g_originalFlashRenderInternal),
                        "CD3D9Renderer::FlashRenderInternal");
}

bool InstallDialogueCameraHooks()
{
    HMODULE game = GetModuleHandleW(L"WHGame.dll");
    if (!game) {
        Log("WHGame.dll was not loaded; dialogue camera hooks unavailable");
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto vtableAddress = base + GetConfig().dialogueCameraVtableRva;
    if (!ValidateModuleAddress(game, vtableAddress,
                               "dialogue camera vtable")) {
        return false;
    }

    void* activate = nullptr;
    void* deactivate = nullptr;
    if (!TryReadPointer(reinterpret_cast<void*>(vtableAddress + 3 * sizeof(void*)),
                        activate) ||
        !TryReadPointer(reinterpret_cast<void*>(vtableAddress + 4 * sizeof(void*)),
                        deactivate) ||
        !ValidateModuleAddress(game, reinterpret_cast<std::uintptr_t>(activate),
                               "dialogue camera Activate") ||
        !ValidateModuleAddress(game, reinterpret_cast<std::uintptr_t>(deactivate),
                               "dialogue camera Deactivate")) {
        Log("Could not resolve dialogue camera state functions");
        return false;
    }

    constexpr std::array<std::uint8_t, 10> activateSignature{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x50};
    constexpr std::array<std::uint8_t, 6> deactivateSignature{
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x30};
    if (std::memcmp(activate, activateSignature.data(), activateSignature.size()) != 0 ||
        std::memcmp(deactivate, deactivateSignature.data(),
                    deactivateSignature.size()) != 0) {
        Log("Dialogue camera signature mismatch; refusing unsafe hooks");
        return false;
    }

    g_dialogueCameraActivateTarget = activate;
    g_dialogueCameraDeactivateTarget = deactivate;
    bool installed = HookFunction(
        g_dialogueCameraActivateTarget,
        reinterpret_cast<void*>(&HookDialogueCameraActivate),
        reinterpret_cast<void**>(&g_originalDialogueCameraActivate),
        "C_DialogCameraManager::Activate");
    installed &= HookFunction(
        g_dialogueCameraDeactivateTarget,
        reinterpret_cast<void*>(&HookDialogueCameraDeactivate),
        reinterpret_cast<void**>(&g_originalDialogueCameraDeactivate),
        "C_DialogCameraManager::Deactivate");
    return installed;
}

}  // namespace

bool InstallHooks()
{
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MinHook initialization failed: %s", MH_StatusToString(status));
        return false;
    }
    const bool d3d11 = InstallD3D11DeviceHooks();
    const bool dxgi = InstallDxgiHook();
    const bool camera = InstallCameraHook();
    const bool stereoCamera = InstallStereoCameraHook();
    const bool hudRender = InstallHudRenderHook();
    const bool dialogueCamera = InstallDialogueCameraHooks();
    OpenXRRuntime::Instance().SetOpenXRProjectionEnabled(stereoCamera &&
                                                         GetConfig().enableOpenXRProjection);
    OpenXRRuntime::Instance().SetHudSeparationEnabled(hudRender &&
                                                      GetConfig().enableHudQuad);
    if (!stereoCamera) {
        Log("OpenXR projection hook unavailable; base stereo submission remains active");
    }
    if (!hudRender) {
        Log("HUD separation hook unavailable; embedded stereo UI remains active");
    }
    if (!dialogueCamera) {
        Log("Dialogue flat-screen switching is unavailable");
    }
    if (!d3d11) {
        Log("Early D3D11 device hook unavailable; pre-swapchain shaders may be missed");
    }
    return dxgi && camera;
}

void RemoveHooks()
{
    OpenXRRuntime::Instance().Shutdown();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

}  // namespace kcdvr
