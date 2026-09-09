#include "DlssUpscaler.h"

#include "Config.h"
#include "Log.h"

#include <Windows.h>
#include <d3d11_1.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace kcdvr {
namespace {

using Microsoft::WRL::ComPtr;

std::atomic<float> g_eyeNear[2] = {0.1F, 0.1F};
std::atomic<float> g_eyeFar[2] = {10000.0F, 10000.0F};

struct PostAaShaderInfo {
    UINT colorSlot = 0;
    UINT motionSlot = 3;
    UINT depthSlot = 16;
    UINT reprojectionConstantSlot = 0;
    UINT reprojectionOffset = 0;
    bool valid = false;
    bool smaa2Tx = false;
    bool smaa1Tx = false;
    bool loggedBoundResources = false;
    std::string color0Name;
    std::string color4Name;
    std::string motionName;
    std::string depthName;
};

struct CapturedTexture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    D3D11_TEXTURE2D_DESC description{};
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
};

struct CapturedEye {
    CapturedTexture color;
    CapturedTexture depth;
    CapturedTexture encodedMotion;
    ComPtr<ID3D11Buffer> reprojectionConstants;
    D3D11_BUFFER_DESC constantDescription{};
    UINT reprojectionOffset = 0;
    std::uint64_t frameIndex = 0;
    bool valid = false;
    bool denseMotionPrepared = false;
    bool colorIsPreSmaa = false;
    bool logged = false;
    bool loggedConstants = false;
};

struct MotionReplayState {
    std::array<ComPtr<ID3D11RenderTargetView>,
               D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
        renderTargets;
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11BlendState> blendState;
    ComPtr<ID3D11Buffer> pixelConstant1;
    std::array<float, 4> blendFactor{};
    UINT sampleMask = 0xFFFFFFFFU;
    ID3D11PixelShader* sourceShader = nullptr;
    std::uint64_t frameIndex = 0;
    int eye = -1;
    bool active = false;
    bool completed = false;
    bool replacedNativeTemporalAa = false;
};

thread_local MotionReplayState t_motionReplay;

class ContextStateScope final {
public:
    ContextStateScope(ID3D11DeviceContext1* context,
                      ID3DDeviceContextState* isolatedState)
        : m_context(context)
    {
        if (m_context && isolatedState) {
            m_context->SwapDeviceContextState(isolatedState, &m_previousState);
            m_active = true;
        }
    }

    ~ContextStateScope()
    {
        if (m_active) {
            m_context->SwapDeviceContextState(m_previousState.Get(), nullptr);
        }
    }

    explicit operator bool() const { return m_active; }

private:
    ComPtr<ID3D11DeviceContext1> m_context;
    ComPtr<ID3DDeviceContextState> m_previousState;
    bool m_active = false;
};

float Halton(std::uint32_t index, std::uint32_t base)
{
    float result = 0.0F;
    float fraction = 1.0F;
    while (index > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

std::uint32_t Crc32C(const void* data, std::size_t size)
{
    std::uint32_t crc = 0xFFFFFFFFU;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0x82F63B78U & mask);
        }
    }
    return ~crc;
}

void LogShaderTexture(ID3D11DeviceContext* context, UINT slot,
                      const char* reflectedName)
{
    ComPtr<ID3D11ShaderResourceView> view;
    context->PSGetShaderResources(slot, 1, &view);
    if (!view) {
        Log("DLSS TRACE: t%u (%s) is unbound", slot,
            reflectedName ? reflectedName : "unnamed");
        return;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    view->GetDesc(&viewDescription);
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) {
        Log("DLSS TRACE: t%u (%s) is not Texture2D (view format=%d)", slot,
            reflectedName ? reflectedName : "unnamed",
            static_cast<int>(viewDescription.Format));
        return;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    Log("DLSS TRACE: t%u (%s)=%ux%u resourceFmt=%d viewFmt=%d bind=0x%X",
        slot, reflectedName ? reflectedName : "unnamed", description.Width,
        description.Height, static_cast<int>(description.Format),
        static_cast<int>(viewDescription.Format), description.BindFlags);
}

bool ReferenceShaderTexture(ID3D11ShaderResourceView* sourceView,
                            CapturedTexture& destination)
{
    if (!sourceView) {
        return false;
    }
    ComPtr<ID3D11Resource> resource;
    sourceView->GetResource(&resource);
    ComPtr<ID3D11Texture2D> source;
    if (FAILED(resource.As(&source))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDescription{};
    D3D11_SHADER_RESOURCE_VIEW_DESC sourceViewDescription{};
    source->GetDesc(&sourceDescription);
    sourceView->GetDesc(&sourceViewDescription);
    if (sourceDescription.SampleDesc.Count != 1 || sourceDescription.ArraySize != 1) {
        return false;
    }

    destination.texture = source;
    destination.view = sourceView;
    destination.description = sourceDescription;
    destination.viewDescription = sourceViewDescription;
    return true;
}

bool ReferenceConstantBuffer(ID3D11Buffer* source, CapturedEye& destination)
{
    if (!source) {
        return false;
    }
    D3D11_BUFFER_DESC description{};
    source->GetDesc(&description);
    if (description.ByteWidth == 0) {
        return false;
    }
    destination.reprojectionConstants = source;
    destination.constantDescription = description;
    return true;
}

bool DumpRgbaTexture(ID3D11Device* device, ID3D11DeviceContext* context,
                     ID3D11Texture2D* source,
                     const std::filesystem::path& destination)
{
    if (!device || !context || !source) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    const bool rgba = description.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                      description.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                      description.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                      description.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                      description.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if ((!rgba && !bgra) || description.ArraySize != 1 ||
        description.SampleDesc.Count != 1) {
        return false;
    }
    D3D11_TEXTURE2D_DESC stagingDescription = description;
    stagingDescription.MipLevels = 1;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDescription, nullptr, &staging))) {
        return false;
    }
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    std::ofstream file(destination, std::ios::binary | std::ios::trunc);
    if (!file) {
        context->Unmap(staging.Get(), 0);
        return false;
    }
    file << "P6\n" << description.Width << ' ' << description.Height << "\n255\n";
    std::vector<unsigned char> row(static_cast<std::size_t>(description.Width) * 3U);
    for (UINT y = 0; y < description.Height; ++y) {
        const auto* sourceRow = static_cast<const unsigned char*>(mapped.pData) +
                                static_cast<std::size_t>(mapped.RowPitch) * y;
        for (UINT x = 0; x < description.Width; ++x) {
            const auto* pixel = sourceRow + static_cast<std::size_t>(x) * 4U;
            row[static_cast<std::size_t>(x) * 3U + 0U] = bgra ? pixel[2] : pixel[0];
            row[static_cast<std::size_t>(x) * 3U + 1U] = pixel[1];
            row[static_cast<std::size_t>(x) * 3U + 2U] = bgra ? pixel[0] : pixel[2];
        }
        file.write(reinterpret_cast<const char*>(row.data()),
                   static_cast<std::streamsize>(row.size()));
    }
    context->Unmap(staging.Get(), 0);
    return file.good();
}

const char* QualityName(int quality)
{
    switch (quality) {
    case 0: return "Performance";
    case 1: return "Balanced";
    case 2: return "Quality";
    case 3: return "UltraPerformance";
    case 5: return "DLAA";
    default: return "Quality";
    }
}

NVSDK_NGX_PerfQuality_Value QualityValue(int quality)
{
    switch (quality) {
    case 0: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case 1: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case 2: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case 3: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    case 5: return NVSDK_NGX_PerfQuality_Value_DLAA;
    default: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
}

const char* RenderPresetName(int preset)
{
    switch (preset) {
    case NVSDK_NGX_DLSS_Hint_Render_Preset_J: return "J";
    case NVSDK_NGX_DLSS_Hint_Render_Preset_K: return "K";
    case NVSDK_NGX_DLSS_Hint_Render_Preset_L: return "L";
    case NVSDK_NGX_DLSS_Hint_Render_Preset_M: return "M";
    default: return "Default";
    }
}

void ApplyRenderPresetHints(NVSDK_NGX_Parameter* parameters, int preset)
{
    if (!parameters) {
        return;
    }
    parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset);
    parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset);
    parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset);
    parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                    preset);
    parameters->Set(
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, preset);
    parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
                    preset);
}

std::uint32_t JitterPhaseCount(int quality)
{
    switch (quality) {
    case 0: return 32;  // Performance
    case 1: return 24;  // Balanced
    case 2: return 18;  // Quality
    case 3: return 72;  // Ultra Performance
    case 5: return 8;   // DLAA
    default: return 18;
    }
}

constexpr std::size_t kGpuProfileRingSize = 8;
constexpr std::uint32_t kGpuProfileLogInterval = 120;

struct GpuFrameProfile {
    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> frameStart;
    std::array<ComPtr<ID3D11Query>, 2> motionStart;
    std::array<ComPtr<ID3D11Query>, 2> motionEnd;
    std::array<ComPtr<ID3D11Query>, 2> dlssStart;
    std::array<ComPtr<ID3D11Query>, 2> dlssEnd;
    ComPtr<ID3D11Query> outputCopyStart;
    ComPtr<ID3D11Query> outputCopyEnd;
    ComPtr<ID3D11Query> frameEnd;
    std::uint64_t frameIndex = 0;
    std::array<bool, 2> motionStarted{};
    std::array<bool, 2> motionIssued{};
    std::array<bool, 2> dlssStarted{};
    std::array<bool, 2> dlssIssued{};
    bool outputCopyStarted = false;
    bool outputCopyIssued = false;
    bool recording = false;
    bool pending = false;
};

struct GpuProfileTotals {
    double frame = 0.0;
    std::array<double, 2> motion{};
    std::array<double, 2> dlss{};
    double outputCopy = 0.0;
    double other = 0.0;
    std::uint32_t samples = 0;
};

}  // namespace

struct DlssUpscaler::Impl {
    mutable std::mutex mutex;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3DDeviceContextState> isolatedContextState;
    std::unordered_map<ID3D11PixelShader*, PostAaShaderInfo> postAaShaders;
    std::array<std::atomic<ID3D11PixelShader*>, 16> postAaShaderPointers{};
    std::atomic_uint32_t postAaShaderCount = 0;
    std::array<ComPtr<ID3D11Texture2D>, 2> engineEyeTargets;
    std::array<CapturedEye, 2> captured;
    std::array<CapturedTexture, 2> preSmaaColor;
    std::array<std::uint64_t, 2> preSmaaColorFrame{
        ~std::uint64_t{0}, ~std::uint64_t{0}};
    std::array<ComPtr<ID3D11Texture2D>, 2> denseMotion;
    std::array<ComPtr<ID3D11ShaderResourceView>, 2> denseMotionViews;
    std::array<ComPtr<ID3D11UnorderedAccessView>, 2> denseMotionUavs;
    std::array<ComPtr<ID3D11RenderTargetView>, 2> denseMotionRtvs;
    std::array<ComPtr<ID3D11Texture2D>, 2> resolvedDepth;
    std::array<ComPtr<ID3D11ShaderResourceView>, 2> resolvedDepthViews;
    std::array<ComPtr<ID3D11RenderTargetView>, 2> resolvedDepthRtvs;
    std::array<ComPtr<ID3D11Texture2D>, 2> outputs;
    std::array<NVSDK_NGX_Handle*, 2> features{};
    NVSDK_NGX_Parameter* parameters = nullptr;
    ComPtr<ID3D11ComputeShader> denseMotionShader;
    ComPtr<ID3D11PixelShader> smaa1TxMotionShader;
    ComPtr<ID3D11PixelShader> smaa1TxCombinedShader;
    ComPtr<ID3D11Buffer> denseParameters;
    ComPtr<ID3D11Buffer> motionReplayParameters;
    UINT denseShaderReprojectionOffset = ~0U;
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    std::uint32_t targetWidth = 0;
    std::uint32_t targetHeight = 0;
    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;
    std::uint32_t rejectedSourceWidth = 0;
    std::uint32_t rejectedSourceHeight = 0;
    std::uint32_t rejectedTargetWidth = 0;
    std::uint32_t rejectedTargetHeight = 0;
    DXGI_FORMAT rejectedOutputFormat = DXGI_FORMAT_UNKNOWN;
    bool ngxInitialized = false;
    bool dlssAvailable = false;
    bool loggedNoInputs = false;
    bool loggedDeferredPostAa = false;
    bool resetHistory = true;
    bool jitterActiveForFrame = false;
    std::array<std::uint64_t, 2> outputFrameIndex{
        ~std::uint64_t{0}, ~std::uint64_t{0}};
    std::uint32_t unresolvedEyeDrawLogs = 0;
    std::uint64_t postAaCaptureFrame = ~std::uint64_t{0};
    std::uint32_t postAaCaptureCount = 0;
    std::uint64_t preSmaaCaptureFrame = ~std::uint64_t{0};
    std::uint32_t preSmaaCaptureCount = 0;
    bool loggedPreSmaaColor = false;
    std::array<bool, 2> dumpedProbe{};
    std::uint64_t frameIndex = 0;
    DlssJitter jitter{};
    std::array<GpuFrameProfile, kGpuProfileRingSize> gpuProfiles;
    GpuFrameProfile* activeGpuProfile = nullptr;
    GpuProfileTotals gpuProfileTotals;
    bool loggedGpuProfileFailure = false;

    void ReleaseFeatures()
    {
        for (auto*& feature : features) {
            if (feature) {
                NVSDK_NGX_D3D11_ReleaseFeature(feature);
                feature = nullptr;
            }
        }
        outputs = {};
        denseMotion = {};
        denseMotionViews = {};
        denseMotionUavs = {};
        denseMotionRtvs = {};
        resolvedDepth = {};
        resolvedDepthViews = {};
        resolvedDepthRtvs = {};
        dumpedProbe = {};
        sourceWidth = sourceHeight = targetWidth = targetHeight = 0;
        outputFormat = DXGI_FORMAT_UNKNOWN;
        resetHistory = true;
        outputFrameIndex = {~std::uint64_t{0}, ~std::uint64_t{0}};
    }

    bool InitializeNgx()
    {
        if (ngxInitialized) {
            return dlssAvailable;
        }
        ngxInitialized = true;
        if (!device) {
            return false;
        }
        const std::wstring dataPath = ModuleDirectory().wstring();
        const NVSDK_NGX_Result init = NVSDK_NGX_D3D11_Init_with_ProjectID(
            "f4740f5f-16d5-4f77-b7a1-0b14db4639cb",
            NVSDK_NGX_ENGINE_TYPE_CUSTOM, "KCD1VR-0.1.67",
            dataPath.c_str(), device.Get());
        if (NVSDK_NGX_FAILED(init)) {
            Log("DLSS: NGX initialization failed: 0x%08X", static_cast<unsigned>(init));
            return false;
        }
        const NVSDK_NGX_Result capability =
            NVSDK_NGX_D3D11_GetCapabilityParameters(&parameters);
        if (NVSDK_NGX_FAILED(capability) || !parameters) {
            Log("DLSS: capability query failed: 0x%08X",
                static_cast<unsigned>(capability));
            return false;
        }
        int available = 0;
        parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        dlssAvailable = available != 0;
        if (!dlssAvailable) {
            int needsDriver = 0;
            unsigned int driverMajor = 0;
            unsigned int driverMinor = 0;
            parameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                            &needsDriver);
            parameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor,
                            &driverMajor);
            parameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor,
                            &driverMinor);
            Log("DLSS: unavailable (needsDriver=%d, minimumDriver=%u.%u)",
                needsDriver, driverMajor, driverMinor);
            return false;
        }
        Log("DLSS: NVIDIA NGX Super Resolution is available");
        return true;
    }

    bool EnsureDenseShader(UINT reprojectionOffset)
    {
        if (denseMotionShader && denseShaderReprojectionOffset == reprojectionOffset) {
            return true;
        }
        if ((reprojectionOffset & 15U) != 0) {
            Log("DLSS: mReprojection is not 16-byte aligned (offset=%u)",
                reprojectionOffset);
            return false;
        }
        std::ostringstream source;
        source << "cbuffer SourceConstants : register(b0) {\n";
        if (reprojectionOffset > 0) {
            source << "float4 padding[" << (reprojectionOffset / 16U) << "];\n";
        }
        source << R"(
float4x4 reprojection;
};
cbuffer DenseParameters : register(b1) { uint2 dimensions; float2 unused; };
Texture2D<float2> encodedMotion : register(t0);
Texture2D<float> nativeDepth : register(t1);
RWTexture2D<float2> denseMotion : register(u0);
float2 DecodeMotion(float2 value) {
    // Match KCD's compiled SMAA 2TX resolve exactly.
    value = (value - (127.0 / 255.0)) * 2.0;
    return value * value * sign(value);
}
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= dimensions.x || id.y >= dimensions.y) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(dimensions);
    float2 encoded = encodedMotion.Load(int3(id.xy, 0));
    float2 velocity;
    if (encoded.x == 0.0) {
        float depth = nativeDepth.Load(int3(id.xy, 0));
        float4 previous = mul(float4(uv, depth, 1.0), reprojection);
        velocity = previous.xy / previous.w - uv;
    } else {
        velocity = DecodeMotion(encoded);
    }
    denseMotion[id.xy] = velocity * float2(dimensions);
}
)";
        ComPtr<ID3DBlob> bytecode;
        ComPtr<ID3DBlob> errors;
        const std::string text = source.str();
        const HRESULT compiled = D3DCompile(
            text.data(), text.size(), "KCD1VR-DenseMotion", nullptr, nullptr,
            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &bytecode, &errors);
        if (FAILED(compiled)) {
            Log("DLSS: dense motion shader compilation failed: %s",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) :
                         "unknown error");
            return false;
        }
        denseMotionShader.Reset();
        if (FAILED(device->CreateComputeShader(
                bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr,
                &denseMotionShader))) {
            Log("DLSS: could not create dense motion shader");
            return false;
        }
        denseShaderReprojectionOffset = reprojectionOffset;
        Log("DLSS: dense motion resolve ready (mReprojection offset=%u)",
            reprojectionOffset);
        return true;
    }

    bool EnsureSmaa1TxMotionShader(bool replaceNativeTemporalAa)
    {
        ComPtr<ID3D11PixelShader>& destination =
            replaceNativeTemporalAa ? smaa1TxCombinedShader :
                                      smaa1TxMotionShader;
        if (destination) {
            return true;
        }
        constexpr const char* source = R"(
cbuffer PER_BATCH : register(b0) {
    row_major float4x4 mViewProjPrev;
    float4 vParams;
    float4 WorldViewPos;
    float4 PS_NearFarClipDist;
    float4 PS_ScreenSize;
};
cbuffer DLSS_MOTION_PARAMETERS : register(b1) {
    float dlssNearPlane;
    float dlssFarPlane;
    uint dlssDepthInverted;
    float dlssUnused;
    float2 dlssRenderSize;
    float dlssMotionScale;
    float dlssUnused2;
};
Texture2D<float4> spatialColor : register(t0);
Texture2D<float> nativeDepth : register(t2);
Texture2D<float2> encodedMotion : register(t3);
SamplerState colorSampler : register(s0);
SamplerState depthSampler : register(s2);
SamplerState motionSampler : register(s3);
struct Input {
    float4 position : SV_Position;
    float4 uvAndScale : TEXCOORD0;
    float4 worldRay : TEXCOORD1;
};
float2 DecodeMotion(float2 value) {
    value = (value - (127.0 / 255.0)) * 2.0;
    return value * value * sign(value);
}
struct Output {
#if COMBINED_COLOR
    float4 color : SV_Target0;
    float2 motion : SV_Target1;
    float deviceDepth : SV_Target2;
#else
    float2 motion : SV_Target0;
    float deviceDepth : SV_Target1;
#endif
};
float ToDeviceDepth(float linearDepth) {
    // CryEngine's Z target stores linear distance normalized by the far
    // plane. Its fullscreen CamVec reaches the far-plane corner.
    float viewZ = max(linearDepth * dlssFarPlane, dlssNearPlane);
    float denominator = max(dlssFarPlane - dlssNearPlane, 0.0001);
    if (dlssDepthInverted != 0) {
        return saturate((dlssNearPlane * dlssFarPlane / viewZ -
                         dlssNearPlane) / denominator);
    }
    return saturate((dlssFarPlane -
                     dlssNearPlane * dlssFarPlane / viewZ) / denominator);
}
Output main(Input input) {
    float2 uv = input.uvAndScale.xy;
    float depth = nativeDepth.Sample(depthSampler, uv);
    float3 world = input.worldRay.xyz * depth + WorldViewPos.xyz;
    float4 previous = mul(float4(world, 1.0), mViewProjPrev);
    float2 velocity = previous.xy / previous.w - uv;
    float2 encoded = encodedMotion.Sample(motionSampler, uv);
    if (encoded.x != 0.0) velocity = DecodeMotion(encoded);
    Output output;
#if COMBINED_COLOR
    output.color = spatialColor.Sample(colorSampler, uv);
#endif
    // CryEngine stores half-pixel reciprocals in PS_ScreenSize.zw. DLSS
    // expects low-resolution pixel motion, so use the known render dimensions
    // directly and retain the fullscreen vertex shader's viewport scale.
    output.motion = velocity * input.uvAndScale.zw *
                    dlssRenderSize * dlssMotionScale;
    output.deviceDepth = ToDeviceDepth(depth);
    return output;
}
)";
        ComPtr<ID3DBlob> bytecode;
        ComPtr<ID3DBlob> errors;
        const D3D_SHADER_MACRO macros[]{
            {"COMBINED_COLOR", replaceNativeTemporalAa ? "1" : "0"},
            {nullptr, nullptr}};
        const HRESULT compiled = D3DCompile(
            source, std::strlen(source), "KCD1VR-SMAA1TX-Motion", macros,
            nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &bytecode, &errors);
        if (FAILED(compiled)) {
            Log("DLSS: SMAA 1TX motion shader compilation failed: %s",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) :
                         "unknown error");
            return false;
        }
        if (FAILED(device->CreatePixelShader(
                bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr,
                &destination))) {
            Log("DLSS: could not create SMAA 1TX motion shader");
            return false;
        }
        Log("DLSS: SMAA 1TX %s motion/depth shader is ready",
            replaceNativeTemporalAa ? "combined color +" : "exact");
        return true;
    }

    bool EnsureWorkingTextures(
        std::uint32_t width, std::uint32_t height,
        std::uint32_t outWidth, std::uint32_t outHeight, DXGI_FORMAT format)
    {
        D3D11_TEXTURE2D_DESC motionDescription{};
        motionDescription.Width = width;
        motionDescription.Height = height;
        motionDescription.MipLevels = 1;
        motionDescription.ArraySize = 1;
        motionDescription.Format = DXGI_FORMAT_R16G16_FLOAT;
        motionDescription.SampleDesc.Count = 1;
        motionDescription.Usage = D3D11_USAGE_DEFAULT;
        motionDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                                      D3D11_BIND_UNORDERED_ACCESS |
                                      D3D11_BIND_RENDER_TARGET;

        D3D11_TEXTURE2D_DESC outputDescription{};
        outputDescription.Width = outWidth;
        outputDescription.Height = outHeight;
        outputDescription.MipLevels = 1;
        outputDescription.ArraySize = 1;
        outputDescription.Format = format;
        outputDescription.SampleDesc.Count = 1;
        outputDescription.Usage = D3D11_USAGE_DEFAULT;
        outputDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                                      D3D11_BIND_UNORDERED_ACCESS |
                                      D3D11_BIND_RENDER_TARGET;

        D3D11_TEXTURE2D_DESC depthDescription{};
        depthDescription.Width = width;
        depthDescription.Height = height;
        depthDescription.MipLevels = 1;
        depthDescription.ArraySize = 1;
        depthDescription.Format = DXGI_FORMAT_R32_FLOAT;
        depthDescription.SampleDesc.Count = 1;
        depthDescription.Usage = D3D11_USAGE_DEFAULT;
        depthDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                                     D3D11_BIND_RENDER_TARGET;

        for (std::size_t eye = 0; eye < 2; ++eye) {
            if (FAILED(device->CreateTexture2D(&motionDescription, nullptr,
                                               &denseMotion[eye])) ||
                FAILED(device->CreateShaderResourceView(
                    denseMotion[eye].Get(), nullptr, &denseMotionViews[eye])) ||
                FAILED(device->CreateUnorderedAccessView(
                    denseMotion[eye].Get(), nullptr, &denseMotionUavs[eye])) ||
                FAILED(device->CreateRenderTargetView(
                    denseMotion[eye].Get(), nullptr, &denseMotionRtvs[eye])) ||
                FAILED(device->CreateTexture2D(&depthDescription, nullptr,
                                               &resolvedDepth[eye])) ||
                FAILED(device->CreateShaderResourceView(
                    resolvedDepth[eye].Get(), nullptr,
                    &resolvedDepthViews[eye])) ||
                FAILED(device->CreateRenderTargetView(
                    resolvedDepth[eye].Get(), nullptr,
                    &resolvedDepthRtvs[eye])) ||
                FAILED(device->CreateTexture2D(&outputDescription, nullptr,
                                               &outputs[eye]))) {
                Log("DLSS: failed to create %ux%u -> %ux%u working textures (format=%d)",
                    width, height, outWidth, outHeight, static_cast<int>(format));
                return false;
            }
        }
        D3D11_BUFFER_DESC parameterDescription{};
        parameterDescription.ByteWidth = 16;
        parameterDescription.Usage = D3D11_USAGE_DEFAULT;
        parameterDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&parameterDescription, nullptr,
                                        &denseParameters))) {
            Log("DLSS: failed to create dense motion parameter buffer");
            return false;
        }
        parameterDescription.ByteWidth = 32;
        if (FAILED(device->CreateBuffer(&parameterDescription, nullptr,
                                        &motionReplayParameters))) {
            Log("DLSS: failed to create motion/depth replay parameter buffer");
            return false;
        }
        return true;
    }

    void LogReprojectionConstants(std::uint32_t eye)
    {
        CapturedEye& input = captured[eye];
        if (input.loggedConstants || !input.reprojectionConstants || !device ||
            !context || input.constantDescription.ByteWidth < 128) {
            return;
        }
        D3D11_BUFFER_DESC stagingDescription = input.constantDescription;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDescription.MiscFlags = 0;
        stagingDescription.StructureByteStride = 0;
        ComPtr<ID3D11Buffer> staging;
        if (FAILED(device->CreateBuffer(&stagingDescription, nullptr, &staging))) {
            return;
        }
        context->CopyResource(staging.Get(), input.reprojectionConstants.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return;
        }
        const auto* value = static_cast<const float*>(mapped.pData);
        Log("DLSS MATRIX eye %u prev rows: [%.6f %.6f %.6f %.6f] [%.6f %.6f %.6f %.6f] [%.6f %.6f %.6f %.6f] [%.6f %.6f %.6f %.6f]",
            eye, value[0], value[1], value[2], value[3], value[4], value[5],
            value[6], value[7], value[8], value[9], value[10], value[11],
            value[12], value[13], value[14], value[15]);
        Log("DLSS MATRIX eye %u vParams=[%.6f %.6f %.6f %.6f] worldPos=[%.6f %.6f %.6f %.6f] shaderNearFar=[%.6f %.6f %.6f %.6f] screen=[%.6f %.6f %.6f %.6f] cameraNearFar=[%.6f %.6f]",
            eye, value[16], value[17], value[18], value[19], value[20], value[21],
            value[22], value[23], value[24], value[25], value[26], value[27],
            value[28], value[29], value[30], value[31],
            g_eyeNear[eye].load(std::memory_order_acquire),
            g_eyeFar[eye].load(std::memory_order_acquire));
        context->Unmap(staging.Get(), 0);
        input.loggedConstants = true;
    }

    bool ResolveDenseMotion(std::uint32_t eye)
    {
        CapturedEye& input = captured[eye];
        if (input.denseMotionPrepared) {
            return true;
        }
        if (!input.valid || !EnsureDenseShader(input.reprojectionOffset)) {
            return false;
        }
        const std::array<std::uint32_t, 4> dimensions{
            sourceWidth, sourceHeight, 0, 0};
        context->UpdateSubresource(denseParameters.Get(), 0, nullptr,
                                   dimensions.data(), 0, 0);
        ID3D11ShaderResourceView* inputs[]{input.encodedMotion.view.Get(),
                                           input.depth.view.Get()};
        ID3D11Buffer* constants[]{input.reprojectionConstants.Get(),
                                  denseParameters.Get()};
        ID3D11UnorderedAccessView* output = denseMotionUavs[eye].Get();
        context->CSSetShader(denseMotionShader.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 2, inputs);
        context->CSSetConstantBuffers(0, 2, constants);
        context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        context->Dispatch((sourceWidth + 7U) / 8U,
                          (sourceHeight + 7U) / 8U, 1);
        ID3D11ShaderResourceView* nullInputs[2]{};
        ID3D11Buffer* nullConstants[2]{};
        ID3D11UnorderedAccessView* nullOutput = nullptr;
        context->CSSetShaderResources(0, 2, nullInputs);
        context->CSSetConstantBuffers(0, 2, nullConstants);
        context->CSSetUnorderedAccessViews(0, 1, &nullOutput, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);
        return true;
    }

    bool EnsureGpuProfileQueries(GpuFrameProfile& profile)
    {
        if (profile.disjoint) {
            return true;
        }
        D3D11_QUERY_DESC description{};
        description.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(device->CreateQuery(&description, &profile.disjoint))) {
            return false;
        }
        description.Query = D3D11_QUERY_TIMESTAMP;
        const auto createTimestamp = [&](ComPtr<ID3D11Query>& query) {
            return SUCCEEDED(device->CreateQuery(&description, &query));
        };
        if (!createTimestamp(profile.frameStart) ||
            !createTimestamp(profile.motionStart[0]) ||
            !createTimestamp(profile.motionStart[1]) ||
            !createTimestamp(profile.motionEnd[0]) ||
            !createTimestamp(profile.motionEnd[1]) ||
            !createTimestamp(profile.dlssStart[0]) ||
            !createTimestamp(profile.dlssStart[1]) ||
            !createTimestamp(profile.dlssEnd[0]) ||
            !createTimestamp(profile.dlssEnd[1]) ||
            !createTimestamp(profile.outputCopyStart) ||
            !createTimestamp(profile.outputCopyEnd) ||
            !createTimestamp(profile.frameEnd)) {
            profile = {};
            return false;
        }
        return true;
    }

    bool ReadTimestamp(ID3D11Query* query, std::uint64_t& value) const
    {
        return query &&
               context->GetData(query, &value, sizeof(value),
                                D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
    }

    void PollGpuProfiles()
    {
        for (GpuFrameProfile& profile : gpuProfiles) {
            if (!profile.pending) {
                continue;
            }
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            if (context->GetData(profile.disjoint.Get(), &disjoint,
                                 sizeof(disjoint),
                                 D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
                continue;
            }
            profile.pending = false;
            if (disjoint.Disjoint || disjoint.Frequency == 0 ||
                !profile.motionIssued[0] || !profile.motionIssued[1] ||
                !profile.dlssIssued[0] || !profile.dlssIssued[1] ||
                !profile.outputCopyIssued) {
                continue;
            }

            std::uint64_t frameStartValue = 0;
            std::uint64_t frameEndValue = 0;
            std::array<std::uint64_t, 2> motionStartValue{};
            std::array<std::uint64_t, 2> motionEndValue{};
            std::array<std::uint64_t, 2> dlssStartValue{};
            std::array<std::uint64_t, 2> dlssEndValue{};
            std::uint64_t outputCopyStartValue = 0;
            std::uint64_t outputCopyEndValue = 0;
            bool ready = ReadTimestamp(profile.frameStart.Get(), frameStartValue) &&
                         ReadTimestamp(profile.frameEnd.Get(), frameEndValue) &&
                         ReadTimestamp(profile.outputCopyStart.Get(),
                                       outputCopyStartValue) &&
                         ReadTimestamp(profile.outputCopyEnd.Get(),
                                       outputCopyEndValue);
            for (std::size_t eye = 0; eye < 2 && ready; ++eye) {
                ready = ReadTimestamp(profile.motionStart[eye].Get(),
                                      motionStartValue[eye]) &&
                        ReadTimestamp(profile.motionEnd[eye].Get(),
                                      motionEndValue[eye]) &&
                        ReadTimestamp(profile.dlssStart[eye].Get(),
                                      dlssStartValue[eye]) &&
                        ReadTimestamp(profile.dlssEnd[eye].Get(),
                                      dlssEndValue[eye]);
            }
            if (!ready || frameEndValue < frameStartValue ||
                outputCopyEndValue < outputCopyStartValue) {
                continue;
            }

            const double milliseconds =
                1000.0 / static_cast<double>(disjoint.Frequency);
            const double frameDuration =
                static_cast<double>(frameEndValue - frameStartValue) *
                milliseconds;
            double measuredDuration = 0.0;
            std::array<double, 2> motionDuration{};
            std::array<double, 2> dlssDuration{};
            for (std::size_t eye = 0; eye < 2; ++eye) {
                if (motionEndValue[eye] < motionStartValue[eye] ||
                    dlssEndValue[eye] < dlssStartValue[eye]) {
                    ready = false;
                    break;
                }
                motionDuration[eye] =
                    static_cast<double>(motionEndValue[eye] -
                                        motionStartValue[eye]) *
                    milliseconds;
                dlssDuration[eye] =
                    static_cast<double>(dlssEndValue[eye] -
                                        dlssStartValue[eye]) *
                    milliseconds;
                measuredDuration += motionDuration[eye] + dlssDuration[eye];
            }
            if (!ready) {
                continue;
            }
            const double outputCopyDuration =
                static_cast<double>(outputCopyEndValue -
                                    outputCopyStartValue) * milliseconds;
            measuredDuration += outputCopyDuration;
            gpuProfileTotals.frame += frameDuration;
            for (std::size_t eye = 0; eye < 2; ++eye) {
                gpuProfileTotals.motion[eye] += motionDuration[eye];
                gpuProfileTotals.dlss[eye] += dlssDuration[eye];
            }
            gpuProfileTotals.outputCopy += outputCopyDuration;
            gpuProfileTotals.other +=
                std::max(0.0, frameDuration - measuredDuration);
            ++gpuProfileTotals.samples;
            if (gpuProfileTotals.samples >= kGpuProfileLogInterval) {
                const double divisor =
                    static_cast<double>(gpuProfileTotals.samples);
                Log("DLSS GPU TIMING (%u-frame average): frame=%.3f ms, motion L/R=%.3f/%.3f ms, DLSS L/R=%.3f/%.3f ms, OpenXR copy=%.3f ms, other game+HUD=%.3f ms",
                    gpuProfileTotals.samples,
                    gpuProfileTotals.frame / divisor,
                    gpuProfileTotals.motion[0] / divisor,
                    gpuProfileTotals.motion[1] / divisor,
                    gpuProfileTotals.dlss[0] / divisor,
                    gpuProfileTotals.dlss[1] / divisor,
                    gpuProfileTotals.outputCopy / divisor,
                    gpuProfileTotals.other / divisor);
                gpuProfileTotals = {};
            }
        }
    }

    void EndGpuFrame()
    {
        if (!activeGpuProfile || !activeGpuProfile->recording || !context) {
            return;
        }
        context->End(activeGpuProfile->frameEnd.Get());
        context->End(activeGpuProfile->disjoint.Get());
        activeGpuProfile->recording = false;
        activeGpuProfile->pending = true;
        activeGpuProfile = nullptr;
    }

    void BeginGpuFrame()
    {
        if (!context || !device) {
            return;
        }
        EndGpuFrame();
        PollGpuProfiles();
        GpuFrameProfile& profile =
            gpuProfiles[frameIndex % gpuProfiles.size()];
        if (profile.pending) {
            return;
        }
        if (!EnsureGpuProfileQueries(profile)) {
            if (!loggedGpuProfileFailure) {
                Log("DLSS GPU TIMING: D3D11 timestamp query creation failed");
                loggedGpuProfileFailure = true;
            }
            return;
        }
        profile.frameIndex = frameIndex;
        profile.motionStarted = {};
        profile.motionIssued = {};
        profile.dlssStarted = {};
        profile.dlssIssued = {};
        profile.outputCopyStarted = false;
        profile.outputCopyIssued = false;
        profile.recording = true;
        context->Begin(profile.disjoint.Get());
        context->End(profile.frameStart.Get());
        activeGpuProfile = &profile;
    }

    void BeginMotionTiming(std::uint32_t eye)
    {
        if (eye < 2 && activeGpuProfile && activeGpuProfile->recording &&
            !activeGpuProfile->motionStarted[eye]) {
            context->End(activeGpuProfile->motionStart[eye].Get());
            activeGpuProfile->motionStarted[eye] = true;
        }
    }

    void EndMotionTiming(std::uint32_t eye)
    {
        if (eye < 2 && activeGpuProfile && activeGpuProfile->recording &&
            activeGpuProfile->motionStarted[eye] &&
            !activeGpuProfile->motionIssued[eye]) {
            context->End(activeGpuProfile->motionEnd[eye].Get());
            activeGpuProfile->motionIssued[eye] = true;
        }
    }

    void BeginDlssTiming(std::uint32_t eye)
    {
        if (eye < 2 && activeGpuProfile && activeGpuProfile->recording &&
            !activeGpuProfile->dlssStarted[eye]) {
            context->End(activeGpuProfile->dlssStart[eye].Get());
            activeGpuProfile->dlssStarted[eye] = true;
        }
    }

    void EndDlssTiming(std::uint32_t eye)
    {
        if (eye < 2 && activeGpuProfile && activeGpuProfile->recording &&
            activeGpuProfile->dlssStarted[eye] &&
            !activeGpuProfile->dlssIssued[eye]) {
            context->End(activeGpuProfile->dlssEnd[eye].Get());
            activeGpuProfile->dlssIssued[eye] = true;
        }
    }

    bool EvaluateEye(std::uint32_t eye)
    {
        if (eye >= 2 || outputFrameIndex[eye] == frameIndex) {
            return eye < 2;
        }
        if (!features[eye] || !captured[eye].valid ||
            captured[eye].frameIndex != frameIndex) {
            return false;
        }
        // NGX preserves D3D11 immediate-context state. The SMAA 2TX compute
        // fallback needs isolated CS bindings.
        const bool isolateContext = !captured[eye].denseMotionPrepared;
        ContextStateScope contextState(
            context1.Get(),
            isolateContext ? isolatedContextState.Get() : nullptr);
        if (isolateContext && !contextState) {
            return false;
        }
        const D3D11_TEXTURE2D_DESC& color = captured[eye].color.description;
        if (color.Width != sourceWidth || color.Height != sourceHeight ||
            !ResolveDenseMotion(eye)) {
            return false;
        }

        NVSDK_NGX_D3D11_DLSS_Eval_Params evaluation{};
        evaluation.Feature.pInColor = captured[eye].color.texture.Get();
        evaluation.Feature.pInOutput = outputs[eye].Get();
        evaluation.pInDepth = captured[eye].denseMotionPrepared
                                  ? resolvedDepth[eye].Get()
                                  : captured[eye].depth.texture.Get();
        evaluation.pInMotionVectors = denseMotion[eye].Get();
        evaluation.InJitterOffsetX = jitter.x;
        evaluation.InJitterOffsetY = jitter.y;
        evaluation.InRenderSubrectDimensions = {sourceWidth, sourceHeight};
        evaluation.InReset = resetHistory ? 1 : 0;
        evaluation.InMVScaleX = 1.0F;
        evaluation.InMVScaleY = 1.0F;
        evaluation.InPreExposure = 1.0F;
        evaluation.InExposureScale = 1.0F;
        BeginDlssTiming(eye);
        const NVSDK_NGX_Result result = NGX_D3D11_EVALUATE_DLSS_EXT(
            context.Get(), features[eye], parameters, &evaluation);
        EndDlssTiming(eye);
        if (NVSDK_NGX_FAILED(result)) {
            Log("DLSS: evaluation failed for eye %u: 0x%08X", eye,
                static_cast<unsigned>(result));
            return false;
        }
        outputFrameIndex[eye] = frameIndex;
        if (!GetConfig().dlssSubmitOutput && !dumpedProbe[eye]) {
            const auto directory = ModuleDirectory();
            const auto inputPath = directory /
                (L"KCD1VR-dlss-eye" + std::to_wstring(eye) + L"-input.ppm");
            const auto outputPath = directory /
                (L"KCD1VR-dlss-eye" + std::to_wstring(eye) + L"-output.ppm");
            const bool inputSaved = DumpRgbaTexture(
                device.Get(), context.Get(), captured[eye].color.texture.Get(),
                inputPath);
            const bool outputSaved = DumpRgbaTexture(
                device.Get(), context.Get(), outputs[eye].Get(), outputPath);
            if (inputSaved && outputSaved) {
                dumpedProbe[eye] = true;
                Log("DLSS: saved off-screen eye %u input/output probe images",
                    eye);
            } else {
                Log("DLSS: could not save off-screen eye %u probe images", eye);
            }
        }
        if (eye == 1) {
            resetHistory = false;
        }
        return true;
    }
};

DlssUpscaler::DlssUpscaler() : m_impl(new Impl()) {}

DlssUpscaler::~DlssUpscaler()
{
    delete m_impl;
}

DlssUpscaler& DlssUpscaler::Instance()
{
    static DlssUpscaler upscaler;
    return upscaler;
}

bool DlssUpscaler::Requested() const
{
    return GetConfig().enableDlss;
}

void DlssUpscaler::Attach(ID3D11Device* device)
{
    if (!Requested() || !device) {
        return;
    }
    Impl& impl = *m_impl;
    std::scoped_lock lock(impl.mutex);
    if (impl.device.Get() == device) {
        return;
    }
    impl.EndGpuFrame();
    impl.gpuProfiles = {};
    impl.gpuProfileTotals = {};
    if (impl.ngxInitialized && impl.device) {
        impl.ReleaseFeatures();
        if (impl.parameters) {
            NVSDK_NGX_D3D11_DestroyParameters(impl.parameters);
            impl.parameters = nullptr;
        }
        NVSDK_NGX_D3D11_Shutdown1(impl.device.Get());
    }
    impl.device = device;
    device->GetImmediateContext(&impl.context);
    impl.context1.Reset();
    impl.isolatedContextState.Reset();
    ComPtr<ID3D11Device1> device1;
    D3D_FEATURE_LEVEL selectedFeatureLevel{};
    const D3D_FEATURE_LEVEL requestedFeatureLevel = device->GetFeatureLevel();
    const HRESULT contextResult = impl.context.As(&impl.context1);
    const HRESULT deviceResult = impl.device.As(&device1);
    HRESULT stateResult = E_NOINTERFACE;
    if (SUCCEEDED(contextResult) && SUCCEEDED(deviceResult)) {
        stateResult = device1->CreateDeviceContextState(
            0, &requestedFeatureLevel, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device), &selectedFeatureLevel,
            &impl.isolatedContextState);
    }
    if (FAILED(contextResult) || FAILED(deviceResult) || FAILED(stateResult)) {
        Log("DLSS: D3D11 context-state isolation is unavailable (context=0x%08X device=0x%08X state=0x%08X); evaluation disabled to protect CryEngine state",
            static_cast<unsigned>(contextResult),
            static_cast<unsigned>(deviceResult),
            static_cast<unsigned>(stateResult));
        return;
    }
    Log("DLSS: isolated D3D11 context state created (feature level=0x%X)",
        static_cast<unsigned>(selectedFeatureLevel));
    impl.ngxInitialized = false;
    impl.dlssAvailable = false;
    impl.InitializeNgx();
}

bool DlssUpscaler::Prepare(std::uint32_t sourceWidth,
                           std::uint32_t sourceHeight,
                           std::uint32_t targetWidth,
                           std::uint32_t targetHeight,
                           DXGI_FORMAT outputFormat)
{
    if (!Requested() || sourceWidth == 0 || sourceHeight == 0 ||
        targetWidth == 0 || targetHeight == 0) {
        return false;
    }
    Impl& impl = *m_impl;
    std::scoped_lock lock(impl.mutex);
    ContextStateScope contextState(impl.context1.Get(),
                                   impl.isolatedContextState.Get());
    if (!contextState) {
        return false;
    }
    if (impl.rejectedSourceWidth == sourceWidth &&
        impl.rejectedSourceHeight == sourceHeight &&
        impl.rejectedTargetWidth == targetWidth &&
        impl.rejectedTargetHeight == targetHeight &&
        impl.rejectedOutputFormat == outputFormat) {
        return false;
    }
    if (!impl.InitializeNgx()) {
        return false;
    }
    if (impl.features[0] && impl.sourceWidth == sourceWidth &&
        impl.sourceHeight == sourceHeight && impl.targetWidth == targetWidth &&
        impl.targetHeight == targetHeight && impl.outputFormat == outputFormat) {
        return true;
    }

    unsigned int optimalWidth = 0;
    unsigned int optimalHeight = 0;
    unsigned int maximumWidth = 0;
    unsigned int maximumHeight = 0;
    unsigned int minimumWidth = 0;
    unsigned int minimumHeight = 0;
    float sharpness = 0.0F;
    const auto quality = QualityValue(GetConfig().dlssQuality);
    const NVSDK_NGX_Result settings = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        impl.parameters, targetWidth, targetHeight, quality,
        &optimalWidth, &optimalHeight, &maximumWidth, &maximumHeight,
        &minimumWidth, &minimumHeight, &sharpness);
    Log("DLSS: %s requested %ux%u -> %ux%u; NGX optimal=%ux%u range=%ux%u..%ux%u result=0x%08X",
        QualityName(GetConfig().dlssQuality), sourceWidth, sourceHeight,
        targetWidth, targetHeight, optimalWidth, optimalHeight,
        minimumWidth, minimumHeight, maximumWidth, maximumHeight,
        static_cast<unsigned>(settings));
    if (NVSDK_NGX_FAILED(settings) || sourceWidth < minimumWidth ||
        sourceHeight < minimumHeight || sourceWidth > maximumWidth ||
        sourceHeight > maximumHeight) {
        Log("DLSS: render size is outside NGX's valid range; using native OpenXR submission until the size changes");
        impl.rejectedSourceWidth = sourceWidth;
        impl.rejectedSourceHeight = sourceHeight;
        impl.rejectedTargetWidth = targetWidth;
        impl.rejectedTargetHeight = targetHeight;
        impl.rejectedOutputFormat = outputFormat;
        impl.ReleaseFeatures();
        return false;
    }

    impl.ReleaseFeatures();
    const bool resourcesReady = impl.EnsureWorkingTextures(
        sourceWidth, sourceHeight, targetWidth, targetHeight, outputFormat);
    ApplyRenderPresetHints(impl.parameters, GetConfig().dlssRenderPreset);
    Log("DLSS: render preset hint=%s (%d)",
        RenderPresetName(GetConfig().dlssRenderPreset),
        GetConfig().dlssRenderPreset);

    bool featuresReady = resourcesReady;
    for (std::size_t eye = 0; featuresReady && eye < 2; ++eye) {
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = sourceWidth;
        create.Feature.InHeight = sourceHeight;
        create.Feature.InTargetWidth = targetWidth;
        create.Feature.InTargetHeight = targetHeight;
        create.Feature.InPerfQualityValue = quality;
        create.InFeatureCreateFlags =
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
            NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
        if (GetConfig().dlssDepthInverted) {
            create.InFeatureCreateFlags |=
                NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        }
        const NVSDK_NGX_Result result = NGX_D3D11_CREATE_DLSS_EXT(
            impl.context.Get(), &impl.features[eye], impl.parameters, &create);
        if (NVSDK_NGX_FAILED(result) || !impl.features[eye]) {
            Log("DLSS: full-frame feature creation failed for eye %zu: 0x%08X",
                eye, static_cast<unsigned>(result));
            featuresReady = false;
        }
    }
    if (!featuresReady) {
        impl.rejectedSourceWidth = sourceWidth;
        impl.rejectedSourceHeight = sourceHeight;
        impl.rejectedTargetWidth = targetWidth;
        impl.rejectedTargetHeight = targetHeight;
        impl.rejectedOutputFormat = outputFormat;
        impl.ReleaseFeatures();
        return false;
    }
    impl.sourceWidth = sourceWidth;
    impl.sourceHeight = sourceHeight;
    impl.targetWidth = targetWidth;
    impl.targetHeight = targetHeight;
    impl.outputFormat = outputFormat;
    impl.rejectedSourceWidth = impl.rejectedSourceHeight = 0;
    impl.rejectedTargetWidth = impl.rejectedTargetHeight = 0;
    impl.rejectedOutputFormat = DXGI_FORMAT_UNKNOWN;
    impl.resetHistory = true;
    Log("DLSS: two independent full-frame VR eye features created");
    return true;
}

bool DlssUpscaler::UpscaleEye(std::uint32_t eye)
{
    if (eye >= 2 || !Requested()) {
        return false;
    }
    Impl& impl = *m_impl;
    std::scoped_lock lock(impl.mutex);
    if (impl.outputFrameIndex[eye] == impl.frameIndex) {
        return true;
    }
    if (!impl.features[eye] || !impl.captured[eye].valid ||
        impl.captured[eye].frameIndex != impl.frameIndex) {
        if (!impl.loggedNoInputs) {
            Log("DLSS: waiting for KCD PostAA color/depth/motion inputs");
            impl.loggedNoInputs = true;
        }
        return false;
    }
    return impl.EvaluateEye(eye);
}

ID3D11Texture2D* DlssUpscaler::OutputTexture(std::uint32_t eye) const
{
    if (!m_impl || eye >= 2) {
        return nullptr;
    }
    return m_impl->outputs[eye].Get();
}

bool DlssUpscaler::InputsReady() const
{
    if (!m_impl || !Requested()) {
        return false;
    }
    std::scoped_lock lock(m_impl->mutex);
    return m_impl->features[0] && m_impl->features[1] &&
           m_impl->outputFrameIndex[0] == m_impl->frameIndex &&
           m_impl->outputFrameIndex[1] == m_impl->frameIndex;
}

void DlssUpscaler::BeginFrame()
{
    if (!Requested()) {
        return;
    }
    Impl& impl = *m_impl;
    std::scoped_lock lock(impl.mutex);
    ++impl.frameIndex;
    impl.jitterActiveForFrame = impl.features[0] && impl.features[1] &&
                                impl.captured[0].valid &&
                                impl.captured[1].valid;
    const std::uint32_t phaseCount = JitterPhaseCount(GetConfig().dlssQuality);
    const std::uint32_t phase =
        static_cast<std::uint32_t>(impl.frameIndex % phaseCount) + 1U;
    impl.jitter.x = (Halton(phase, 2) - 0.5F) * GetConfig().dlssJitterScaleX;
    impl.jitter.y = (Halton(phase, 3) - 0.5F) * GetConfig().dlssJitterScaleY;
    if (GetConfig().dlssGpuTiming) {
        impl.BeginGpuFrame();
    }
}

void DlssUpscaler::BeginOutputCopyTiming()
{
    if (!m_impl || !Requested()) {
        return;
    }
    std::scoped_lock lock(m_impl->mutex);
    Impl& impl = *m_impl;
    if (impl.activeGpuProfile && impl.activeGpuProfile->recording &&
        !impl.activeGpuProfile->outputCopyStarted) {
        impl.context->End(impl.activeGpuProfile->outputCopyStart.Get());
        impl.activeGpuProfile->outputCopyStarted = true;
    }
}

void DlssUpscaler::EndOutputCopyTiming()
{
    if (!m_impl || !Requested()) {
        return;
    }
    std::scoped_lock lock(m_impl->mutex);
    Impl& impl = *m_impl;
    if (impl.activeGpuProfile && impl.activeGpuProfile->recording &&
        impl.activeGpuProfile->outputCopyStarted &&
        !impl.activeGpuProfile->outputCopyIssued) {
        impl.context->End(impl.activeGpuProfile->outputCopyEnd.Get());
        impl.activeGpuProfile->outputCopyIssued = true;
    }
}

void DlssUpscaler::EndFrameGpuTiming()
{
    if (!m_impl || !Requested()) {
        return;
    }
    std::scoped_lock lock(m_impl->mutex);
    m_impl->EndGpuFrame();
}

DlssJitter DlssUpscaler::CurrentJitter() const
{
    if (!m_impl || !Requested()) {
        return {};
    }
    std::scoped_lock lock(m_impl->mutex);
    if (!m_impl->jitterActiveForFrame) {
        return {};
    }
    DlssJitter result = m_impl->jitter;
    result.renderWidth = m_impl->sourceWidth;
    result.renderHeight = m_impl->sourceHeight;
    return result;
}

void DlssUpscaler::Shutdown()
{
    if (!m_impl) {
        return;
    }
    Impl& impl = *m_impl;
    std::scoped_lock lock(impl.mutex);
    impl.EndGpuFrame();
    impl.ReleaseFeatures();
    if (impl.parameters) {
        NVSDK_NGX_D3D11_DestroyParameters(impl.parameters);
        impl.parameters = nullptr;
    }
    if (impl.ngxInitialized && impl.device) {
        NVSDK_NGX_D3D11_Shutdown1(impl.device.Get());
    }
    impl.ngxInitialized = false;
    impl.dlssAvailable = false;
    impl.context.Reset();
    impl.device.Reset();
    impl.gpuProfiles = {};
    impl.gpuProfileTotals = {};
    impl.activeGpuProfile = nullptr;
}

DXGI_FORMAT DlssOutputFormat(DXGI_FORMAT inputFormat)
{
    switch (inputFormat) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        // BGRA formats cannot be bound as D3D11 unordered-access outputs.
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return inputFormat;
    }
}

void SetDlssEyeClipPlanes(int eye, float nearPlane, float farPlane)
{
    if (eye < 0 || eye >= 2 || !(nearPlane > 0.001F) ||
        !(farPlane > nearPlane)) {
        return;
    }
    g_eyeNear[static_cast<std::size_t>(eye)].store(
        nearPlane, std::memory_order_release);
    g_eyeFar[static_cast<std::size_t>(eye)].store(
        farPlane, std::memory_order_release);
}

void SetDlssEyeRenderTargets(ID3D11Texture2D* leftEye,
                             ID3D11Texture2D* rightEye)
{
    if (!GetConfig().enableDlss) {
        return;
    }
    DlssUpscaler::Impl& impl = *DlssUpscaler::Instance().m_impl;
    std::scoped_lock lock(impl.mutex);
    impl.engineEyeTargets[0] = leftEye;
    impl.engineEyeTargets[1] = rightEye;
}

void RegisterDlssPostAaShader(ID3D11PixelShader* shader, const void* bytecode,
                              std::size_t bytecodeLength)
{
    if (!GetConfig().enableDlss || !shader || !bytecode || bytecodeLength == 0) {
        return;
    }
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(bytecode, bytecodeLength, IID_PPV_ARGS(&reflection)))) {
        return;
    }
    PostAaShaderInfo info;
    const std::uint32_t hash = Crc32C(bytecode, bytecodeLength);
    constexpr std::uint32_t smaa1TxResolveHash = 0xCB02CDB7U;
    D3D11_SHADER_INPUT_BIND_DESC binding{};
    D3D11_SHADER_DESC shaderDescription{};
    if (FAILED(reflection->GetDesc(&shaderDescription))) {
        return;
    }
    bool hasDepth = false;
    bool hasMotion = false;
    bool hasColor0 = false;
    bool hasColor4 = false;
    bool hasDepth2 = false;
    for (UINT index = 0; index < shaderDescription.BoundResources; ++index) {
        D3D11_SHADER_INPUT_BIND_DESC resource{};
        if (FAILED(reflection->GetResourceBindingDesc(index, &resource)) ||
            resource.Type != D3D_SIT_TEXTURE) {
            continue;
        }
        hasDepth |= resource.BindPoint == 16;
        hasDepth2 |= resource.BindPoint == 2;
        hasMotion |= resource.BindPoint == 3;
        hasColor0 |= resource.BindPoint == 0;
        hasColor4 |= resource.BindPoint == 4;
        const char* name = resource.Name ? resource.Name : "unnamed";
        if (resource.BindPoint == 0) info.color0Name = name;
        if (resource.BindPoint == 3) info.motionName = name;
        if (resource.BindPoint == 4) info.color4Name = name;
        if (resource.BindPoint == 2) info.depthName = name;
        if (resource.BindPoint == 16) info.depthName = name;
    }
    const bool isSmaa1Tx = hash == smaa1TxResolveHash && hasColor0 &&
                           hasMotion && hasDepth2;
    if ((!hasDepth || !hasMotion || (!hasColor0 && !hasColor4)) &&
        !isSmaa1Tx) {
        return;
    }
    info.depthSlot = isSmaa1Tx ? 2U : 16U;
    info.motionSlot = 3;
    info.smaa1Tx = isSmaa1Tx;
    info.smaa2Tx = !isSmaa1Tx && hasColor4;
    info.colorSlot = info.smaa2Tx ? 4U : 0U;

    std::error_code fileError;
    const std::filesystem::path shaderDirectory =
        ModuleDirectory() / L"KCD1VR-shaders";
    std::filesystem::create_directories(shaderDirectory, fileError);
    std::ostringstream shaderName;
    shaderName << "postaa_" << std::hex << std::setfill('0') << std::setw(8)
               << hash << '_' << std::dec << bytecodeLength << ".dxbc";
    const std::filesystem::path shaderPath = shaderDirectory / shaderName.str();
    if (!fileError) {
        std::ofstream shaderFile(shaderPath, std::ios::binary | std::ios::trunc);
        shaderFile.write(static_cast<const char*>(bytecode),
                         static_cast<std::streamsize>(bytecodeLength));
    }

    if (isSmaa1Tx) {
        info.reprojectionConstantSlot = 0;
        info.reprojectionOffset = 0;
        info.valid = true;
    }
    for (UINT index = 0; !info.valid &&
                         index < shaderDescription.ConstantBuffers; ++index) {
        ID3D11ShaderReflectionConstantBuffer* constants =
            reflection->GetConstantBufferByIndex(index);
        D3D11_SHADER_BUFFER_DESC constantDescription{};
        if (FAILED(constants->GetDesc(&constantDescription))) {
            continue;
        }
        ID3D11ShaderReflectionVariable* variable =
            constants->GetVariableByName("mReprojection");
        D3D11_SHADER_VARIABLE_DESC variableDescription{};
        if (FAILED(variable->GetDesc(&variableDescription)) ||
            variableDescription.Size < 64 ||
            FAILED(reflection->GetResourceBindingDescByName(
                constantDescription.Name, &binding))) {
            continue;
        }
        info.reprojectionConstantSlot = binding.BindPoint;
        info.reprojectionOffset = variableDescription.StartOffset;
        info.valid = true;
        break;
    }
    if (!info.valid) {
        Log("DLSS: PostAA-like shader %p has t0/t3/t16 inputs but no reflected mReprojection matrix",
            shader);
        return;
    }
    DlssUpscaler::Impl& impl = *DlssUpscaler::Instance().m_impl;
    std::scoped_lock lock(impl.mutex);
    impl.postAaShaders[shader] = info;
    const std::uint32_t registeredCount =
        impl.postAaShaderCount.load(std::memory_order_relaxed);
    bool pointerRegistered = false;
    for (std::uint32_t index = 0; index < registeredCount; ++index) {
        pointerRegistered |=
            impl.postAaShaderPointers[index].load(std::memory_order_relaxed) ==
            shader;
    }
    if (!pointerRegistered &&
        registeredCount < impl.postAaShaderPointers.size()) {
        impl.postAaShaderPointers[registeredCount].store(
            shader, std::memory_order_relaxed);
        impl.postAaShaderCount.store(registeredCount + 1,
                                     std::memory_order_release);
    }
    Log("DLSS: recognized KCD PostAA shader %p crc32c=%08x (%s, color=t%u=%s, alternate=t%u=%s, motion=t%u=%s, depth=t%u=%s, constants=b%u+%u, file=%s)",
        shader, hash, info.smaa1Tx ? "SMAA 1TX exact replay" : "SMAA 2TX",
        info.colorSlot,
        info.colorSlot == 4 ? info.color4Name.c_str() : info.color0Name.c_str(),
        info.colorSlot == 4 ? 0U : 4U,
        info.colorSlot == 4 ? info.color0Name.c_str() : info.color4Name.c_str(),
        info.motionSlot, info.motionName.c_str(), info.depthSlot,
        info.depthName.c_str(),
        info.reprojectionConstantSlot, info.reprojectionOffset,
        shaderPath.string().c_str());
    if (info.smaa2Tx) {
        Log("DLSS WARNING: KCD SMAA 2TX is active; its independent temporal jitter is incompatible with DLSS jitter. Select SMAA 1TX in the game for DLSS testing.");
    }
}

ID3D11PixelShader* BeginDlss1TxMotionReplay(ID3D11DeviceContext* context,
                                            ID3D11PixelShader* shader)
{
    t_motionReplay = {};
    if (!GetConfig().enableDlss || !context || !shader) {
        return nullptr;
    }
    DlssUpscaler::Impl& impl = *DlssUpscaler::Instance().m_impl;
    std::scoped_lock lock(impl.mutex);
    const auto found = impl.postAaShaders.find(shader);
    if (found == impl.postAaShaders.end() || !found->second.smaa1Tx ||
        !impl.device || !impl.EnsureSmaa1TxMotionShader(
                            GetConfig().dlssReplaceNativeTemporalAa) ||
        !impl.motionReplayParameters) {
        return nullptr;
    }

    int eyeIndex = -1;
    ComPtr<ID3D11RenderTargetView> currentTarget;
    context->OMGetRenderTargets(1, &currentTarget, nullptr);
    ComPtr<ID3D11Texture2D> currentTexture;
    if (currentTarget) {
        ComPtr<ID3D11Resource> resource;
        currentTarget->GetResource(&resource);
        resource.As(&currentTexture);
    }
    if (currentTexture) {
        for (int candidate = 0; candidate < 2; ++candidate) {
            if (impl.engineEyeTargets[static_cast<std::size_t>(candidate)].Get() ==
                currentTexture.Get()) {
                eyeIndex = candidate;
                break;
            }
        }
    }
    if (eyeIndex < 0) {
        if (impl.postAaCaptureFrame != impl.frameIndex) {
            impl.postAaCaptureFrame = impl.frameIndex;
            impl.postAaCaptureCount = 0;
        }
        if (impl.postAaCaptureCount < 2) {
            eyeIndex = static_cast<int>(impl.postAaCaptureCount++);
        }
    }
    if (eyeIndex < 0 || eyeIndex >= 2 ||
        !impl.denseMotionRtvs[static_cast<std::size_t>(eyeIndex)] ||
        !impl.resolvedDepthRtvs[static_cast<std::size_t>(eyeIndex)]) {
        return nullptr;
    }

    ID3D11RenderTargetView* rawTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* rawDepth = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                rawTargets, &rawDepth);
    for (std::size_t index = 0; index < t_motionReplay.renderTargets.size();
         ++index) {
        t_motionReplay.renderTargets[index].Attach(rawTargets[index]);
    }
    t_motionReplay.depth.Attach(rawDepth);
    ID3D11BlendState* rawBlend = nullptr;
    context->OMGetBlendState(&rawBlend, t_motionReplay.blendFactor.data(),
                             &t_motionReplay.sampleMask);
    t_motionReplay.blendState.Attach(rawBlend);
    ID3D11Buffer* rawConstant1 = nullptr;
    context->PSGetConstantBuffers(1, 1, &rawConstant1);
    t_motionReplay.pixelConstant1.Attach(rawConstant1);
    t_motionReplay.eye = eyeIndex;
    t_motionReplay.frameIndex = impl.frameIndex;
    t_motionReplay.sourceShader = shader;
    t_motionReplay.active = true;
    t_motionReplay.replacedNativeTemporalAa =
        GetConfig().dlssReplaceNativeTemporalAa;

    if (t_motionReplay.replacedNativeTemporalAa) {
        ID3D11RenderTargetView* replayTargets[]{
            t_motionReplay.renderTargets[0].Get(),
            impl.denseMotionRtvs[static_cast<std::size_t>(eyeIndex)].Get(),
            impl.resolvedDepthRtvs[static_cast<std::size_t>(eyeIndex)].Get()};
        context->OMSetRenderTargets(3, replayTargets, nullptr);
    } else {
        ID3D11RenderTargetView* replayTargets[]{
            impl.denseMotionRtvs[static_cast<std::size_t>(eyeIndex)].Get(),
            impl.resolvedDepthRtvs[static_cast<std::size_t>(eyeIndex)].Get()};
        context->OMSetRenderTargets(2, replayTargets, nullptr);
    }
    struct MotionReplayParameters {
        float nearPlane;
        float farPlane;
        std::uint32_t depthInverted;
        float unused;
        float renderWidth;
        float renderHeight;
        float motionScale;
        float unused2;
    };
    const MotionReplayParameters parameters{
        g_eyeNear[static_cast<std::size_t>(eyeIndex)].load(
            std::memory_order_acquire),
        g_eyeFar[static_cast<std::size_t>(eyeIndex)].load(
            std::memory_order_acquire),
        GetConfig().dlssDepthInverted ? 1U : 0U, 0.0F,
        static_cast<float>(impl.sourceWidth),
        static_cast<float>(impl.sourceHeight),
        GetConfig().dlssMotionScale, 0.0F};
    context->UpdateSubresource(impl.motionReplayParameters.Get(), 0, nullptr,
                               &parameters, 0, 0);
    ID3D11Buffer* replayParameters = impl.motionReplayParameters.Get();
    context->PSSetConstantBuffers(1, 1, &replayParameters);
    const float opaqueBlend[4]{};
    context->OMSetBlendState(nullptr, opaqueBlend, 0xFFFFFFFFU);
    if (context == impl.context.Get()) {
        impl.BeginMotionTiming(static_cast<std::uint32_t>(eyeIndex));
    }
    return t_motionReplay.replacedNativeTemporalAa
               ? impl.smaa1TxCombinedShader.Get()
               : impl.smaa1TxMotionShader.Get();
}

void EndDlss1TxMotionReplay(ID3D11DeviceContext* context)
{
    if (!context || !t_motionReplay.active) {
        return;
    }
    {
        DlssUpscaler::Impl& impl = *DlssUpscaler::Instance().m_impl;
        std::scoped_lock lock(impl.mutex);
        if (context == impl.context.Get() && t_motionReplay.eye >= 0 &&
            t_motionReplay.eye < 2 &&
            t_motionReplay.frameIndex == impl.frameIndex) {
            impl.EndMotionTiming(
                static_cast<std::uint32_t>(t_motionReplay.eye));
        }
    }
    ID3D11RenderTargetView* rawTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    for (std::size_t index = 0; index < t_motionReplay.renderTargets.size();
         ++index) {
        rawTargets[index] = t_motionReplay.renderTargets[index].Get();
    }
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                rawTargets, t_motionReplay.depth.Get());
    context->OMSetBlendState(t_motionReplay.blendState.Get(),
                             t_motionReplay.blendFactor.data(),
                             t_motionReplay.sampleMask);
    ID3D11Buffer* originalConstant1 = t_motionReplay.pixelConstant1.Get();
    context->PSSetConstantBuffers(1, 1, &originalConstant1);
    t_motionReplay.active = false;
    t_motionReplay.completed = true;
}

bool IsDlssPostAaShader(ID3D11PixelShader* shader)
{
    if (!GetConfig().enableDlss || !shader) {
        return false;
    }
    DlssUpscaler::Impl& impl = *DlssUpscaler::Instance().m_impl;
    const std::uint32_t count =
        impl.postAaShaderCount.load(std::memory_order_acquire);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (impl.postAaShaderPointers[index].load(std::memory_order_relaxed) ==
            shader) {
            return true;
        }
    }
    return false;
}

void CaptureDlssPreSmaaColor(ID3D11DeviceContext* context)
{
    if (!GetConfig().enableDlss || !context) {
        return;
    }
    DlssUpscaler::Impl& impl = *DlssUpscaler::Instance().m_impl;
    std::scoped_lock lock(impl.mutex);
    if (!impl.device) {
        return;
    }
    if (impl.preSmaaCaptureFrame != impl.frameIndex) {
        impl.preSmaaCaptureFrame = impl.frameIndex;
        impl.preSmaaCaptureCount = 0;
    }
    if (impl.preSmaaCaptureCount >= 2) {
        return;
    }
    ComPtr<ID3D11ShaderResourceView> color;
    // KCD's SMAA neighborhood pass binds its unfiltered scene color at t1.
    context->PSGetShaderResources(1, 1, &color);
    CapturedTexture candidate;
    if (!ReferenceShaderTexture(color.Get(), candidate) ||
        candidate.description.Width == 0 || candidate.description.Height == 0 ||
        candidate.description.SampleDesc.Count != 1) {
        return;
    }
    const std::size_t eye = impl.preSmaaCaptureCount++;
    impl.preSmaaColor[eye] = candidate;
    impl.preSmaaColorFrame[eye] = impl.frameIndex;
    if (!impl.loggedPreSmaaColor) {
        Log("DLSS: captured raw pre-SMAA scene color from neighborhood pass: %ux%u resourceFmt=%d viewFmt=%d",
            candidate.description.Width, candidate.description.Height,
            static_cast<int>(candidate.description.Format),
            static_cast<int>(candidate.viewDescription.Format));
        impl.loggedPreSmaaColor = true;
    }
}

void CaptureDlssPostAaInputs(ID3D11DeviceContext* context,
                             ID3D11PixelShader* shader)
{
    if (!GetConfig().enableDlss || !context || !shader) {
        return;
    }
    DlssUpscaler::Impl& impl = *DlssUpscaler::Instance().m_impl;
    std::scoped_lock lock(impl.mutex);
    const auto found = impl.postAaShaders.find(shader);
    if (found == impl.postAaShaders.end() || !impl.device) {
        return;
    }

    int eyeIndex = -1;
    ComPtr<ID3D11RenderTargetView> renderTarget;
    context->OMGetRenderTargets(1, &renderTarget, nullptr);
    ComPtr<ID3D11Texture2D> targetTexture;
    if (renderTarget) {
        ComPtr<ID3D11Resource> targetResource;
        renderTarget->GetResource(&targetResource);
        targetResource.As(&targetTexture);
    }
    bool matchedTarget = false;
    if (found->second.smaa1Tx && t_motionReplay.completed &&
        t_motionReplay.sourceShader == shader &&
        t_motionReplay.frameIndex == impl.frameIndex) {
        eyeIndex = t_motionReplay.eye;
        matchedTarget = true;
    }
    if (targetTexture) {
        for (int candidate = 0; candidate < 2; ++candidate) {
            if (impl.engineEyeTargets[static_cast<std::size_t>(candidate)].Get() ==
                targetTexture.Get()) {
                eyeIndex = candidate;
                matchedTarget = true;
                break;
            }
        }
    }
    if (!matchedTarget) {
        // KCD ping-pongs several PostAA targets, so texture identity is not a
        // stable eye key. This pass is issued left then right once per stereo
        // frame; assign the first two matching draws in that order.
        if (impl.postAaCaptureFrame != impl.frameIndex) {
            impl.postAaCaptureFrame = impl.frameIndex;
            impl.postAaCaptureCount = 0;
        }
        if (impl.postAaCaptureCount < 2) {
            eyeIndex = static_cast<int>(impl.postAaCaptureCount++);
        }
    }
    if (eyeIndex < 0 || eyeIndex >= 2) {
        if (impl.unresolvedEyeDrawLogs < 4) {
            Log("DLSS: recognized PostAA draw has no eye identity (thread=%lu, rtv=%p)",
                static_cast<unsigned long>(GetCurrentThreadId()),
                targetTexture.Get());
            ++impl.unresolvedEyeDrawLogs;
        }
        return;
    }
    PostAaShaderInfo& info = found->second;
    if (!info.loggedBoundResources) {
        LogShaderTexture(context, 0, info.color0Name.c_str());
        LogShaderTexture(context, 3, info.motionName.c_str());
        LogShaderTexture(context, 4, info.color4Name.c_str());
        LogShaderTexture(context, info.depthSlot, info.depthName.c_str());
        if (targetTexture) {
            D3D11_TEXTURE2D_DESC targetDescription{};
            targetTexture->GetDesc(&targetDescription);
            Log("DLSS TRACE: PostAA target=%ux%u resourceFmt=%d bind=0x%X",
                targetDescription.Width, targetDescription.Height,
                static_cast<int>(targetDescription.Format),
                targetDescription.BindFlags);
        }
        info.loggedBoundResources = true;
    }
    ComPtr<ID3D11ShaderResourceView> color;
    ComPtr<ID3D11ShaderResourceView> motion;
    ComPtr<ID3D11ShaderResourceView> depth;
    context->PSGetShaderResources(info.colorSlot, 1, &color);
    context->PSGetShaderResources(info.motionSlot, 1, &motion);
    context->PSGetShaderResources(info.depthSlot, 1, &depth);
    ComPtr<ID3D11Buffer> constants;
    context->PSGetConstantBuffers(info.reprojectionConstantSlot, 1, &constants);
    if (!color || !motion || !depth || !constants) {
        return;
    }
    CapturedEye& eye = impl.captured[static_cast<std::size_t>(eyeIndex)];
    eye.valid = false;
    eye.denseMotionPrepared = false;
    eye.colorIsPreSmaa = false;
    CapturedTexture postSmaaColor;
    if (!ReferenceShaderTexture(color.Get(), postSmaaColor) ||
        !ReferenceShaderTexture(depth.Get(), eye.depth) ||
        !ReferenceConstantBuffer(constants.Get(), eye) ||
        (!info.smaa1Tx &&
         !ReferenceShaderTexture(motion.Get(), eye.encodedMotion))) {
        return;
    }
    const std::size_t eyeSlot = static_cast<std::size_t>(eyeIndex);
    const CapturedTexture& rawColor = impl.preSmaaColor[eyeSlot];
    if (GetConfig().dlssUseRawPreSmaaColor &&
        impl.preSmaaColorFrame[eyeSlot] == impl.frameIndex && rawColor.texture &&
        rawColor.description.Width == postSmaaColor.description.Width &&
        rawColor.description.Height == postSmaaColor.description.Height) {
        eye.color = rawColor;
        eye.colorIsPreSmaa = true;
    } else {
        eye.color = postSmaaColor;
    }
    eye.reprojectionOffset = info.reprojectionOffset;
    eye.denseMotionPrepared = info.smaa1Tx && t_motionReplay.completed &&
                              t_motionReplay.eye == eyeIndex &&
                              t_motionReplay.frameIndex == impl.frameIndex;
    t_motionReplay.completed = false;
    if (info.smaa1Tx && !eye.denseMotionPrepared) {
        return;
    }
    eye.frameIndex = impl.frameIndex;
    eye.valid = true;
    if (context == impl.context.Get()) {
        impl.LogReprojectionConstants(static_cast<std::uint32_t>(eyeIndex));
    }
    bool evaluatedImmediately = false;
    if (context == impl.context.Get()) {
        evaluatedImmediately =
            impl.EvaluateEye(static_cast<std::uint32_t>(eyeIndex));
    } else if (!impl.loggedDeferredPostAa) {
        Log("DLSS: PostAA was recorded on a deferred D3D11 context; immediate zero-copy evaluation is unavailable for that draw");
        impl.loggedDeferredPostAa = true;
    }
    if (impl.outputFrameIndex[0] == impl.frameIndex &&
        impl.outputFrameIndex[1] == impl.frameIndex) {
        impl.loggedNoInputs = false;
    }
    if (!eye.logged) {
        Log("DLSS: zero-copy eye %d PostAA evaluation %s: color=%ux%u fmt=%d (%s), depth=%s, motion=%s",
            eyeIndex, evaluatedImmediately ? "completed" : "deferred",
            eye.color.description.Width, eye.color.description.Height,
            static_cast<int>(eye.color.viewDescription.Format),
            eye.colorIsPreSmaa ? "raw pre-SMAA" : "spatial SMAA fallback",
            eye.denseMotionPrepared ? "normalized device depth replay" :
                                      "native PostAA depth",
            eye.denseMotionPrepared ? "SMAA 1TX exact replay" :
                                      "SMAA 2TX compute reconstruction");
        eye.logged = true;
    }
}

}  // namespace kcdvr
