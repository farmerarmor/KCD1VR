#include "ShaderCapture.h"

#include "Config.h"
#include "DxbcChecksum.h"
#include "DlssUpscaler.h"
#include "Log.h"

#include <Windows.h>
#include <d3d11.h>
#include <MinHook.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kcdvr {
namespace {

using Microsoft::WRL::ComPtr;

using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using PSSetShaderFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);

CreateVertexShaderFn g_originalCreateVertexShader = nullptr;
CreatePixelShaderFn g_originalCreatePixelShader = nullptr;
PSSetShaderFn g_originalPSSetShader = nullptr;
PSSetShaderFn g_originalPSSetShaderDeferred = nullptr;
DrawIndexedFn g_originalDrawIndexed = nullptr;
DrawFn g_originalDraw = nullptr;
DrawIndexedInstancedFn g_originalDrawIndexedInstanced = nullptr;
DrawInstancedFn g_originalDrawInstanced = nullptr;
DrawAutoFn g_originalDrawAuto = nullptr;
DrawIndirectFn g_originalDrawIndexedInstancedIndirect = nullptr;
DrawIndirectFn g_originalDrawInstancedIndirect = nullptr;
DrawIndirectFn g_originalDrawIndexedInstancedIndirectDeferred = nullptr;
DrawIndirectFn g_originalDrawInstancedIndirectDeferred = nullptr;
void* g_createVertexShaderTarget = nullptr;
void* g_createPixelShaderTarget = nullptr;
std::mutex g_captureMutex;
std::unordered_set<std::uint32_t> g_capturedShaders;
std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> g_retainedShaderBytecode;
std::size_t g_retainedShaderBytecodeBytes = 0;
std::unordered_map<std::uint32_t, std::vector<std::uint8_t>>
    g_retainedVertexShaderBytecode;
std::size_t g_retainedVertexShaderBytecodeBytes = 0;
std::shared_mutex g_shaderMapMutex;
std::unordered_map<ID3D11PixelShader*, std::uint32_t> g_shaderHashes;
std::unordered_map<ID3D11VertexShader*, std::uint32_t> g_vertexShaderHashes;
std::atomic_uint64_t g_shaderMapGeneration = 1;
std::mutex g_browserMutex;
std::unordered_set<std::uint32_t> g_observedShaders;
std::vector<std::uint32_t> g_browserShaders;
std::atomic_bool g_browserCapturing = false;
std::atomic_uint32_t g_selectedSkipHash = 0;
std::atomic_uint32_t g_captureGeneration = 0;
std::atomic_uint64_t g_presentCounter = 0;
std::atomic_uint32_t g_svoTraceCount = 0;
std::atomic_uint32_t g_doorShaderTraceCount = 0;
std::atomic_uint32_t g_dlss1TxTraceCount = 0;
std::atomic_uint64_t g_dlss1TxTraceFirstPresent =
    std::numeric_limits<std::uint64_t>::max();
std::mutex g_dlss1TxTraceMutex;
std::unordered_set<std::uint32_t> g_dlss1TxTracedShaders;
int g_captureFramesRemaining = 0;
std::size_t g_browserIndex = 0;
thread_local std::uint32_t t_currentPixelShaderHash = 0;
thread_local ID3D11PixelShader* t_currentPixelShader = nullptr;
thread_local bool t_currentPixelShaderIsDlssPostAa = false;
struct PixelShaderCacheEntry {
    ID3D11PixelShader* shader = nullptr;
    std::uint32_t hash = 0;
    std::uint64_t generation = 0;
};
thread_local std::array<PixelShaderCacheEntry, 256> t_pixelShaderCache{};
thread_local std::uint32_t t_seenCaptureGeneration = 0;
thread_local std::uint32_t t_lastObservedShaderHash = 0;

constexpr std::string_view kMaterialMarker = "DirtBloodMaskSampler";
constexpr std::string_view kScreenMarker = "PS_ScreenSize";
constexpr std::uint32_t kTargetShaderHash = 0x0001919AU;
constexpr std::uint32_t kSvoTemporalShaderHash = 0x0025B6EFU;
constexpr std::uint32_t kDoorWavyShaderHash = 0xBFF18ADAU;
constexpr std::uint32_t kDlss1TxResolveShaderHash = 0xCB02CDB7U;
constexpr std::uint32_t kDlssPreSmaaColorShaderHash = 0xBC2C0E16U;
constexpr std::size_t kRetainedShaderBytecodeLimit = 64U * 1024U * 1024U;
constexpr std::size_t kRetainedVertexShaderBytecodeLimit = 16U * 1024U * 1024U;
std::atomic_bool g_loggedMotionPatch = false;
std::atomic_bool g_loggedDoorMotionPatch = false;
std::atomic_bool g_loggedSvoHistoryPatch = false;
std::atomic_bool g_loggedShaderRetentionLimit = false;

struct SvoEyeHistory {
    ComPtr<ID3D11Device> device;
    std::array<ComPtr<ID3D11Texture2D>, 4> textures;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> views;
};

struct SvoHistoryOverride {
    bool active = false;
    unsigned eye = 0;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> originalInputs;
    std::array<ComPtr<ID3D11RenderTargetView>, 4> outputs;
    std::array<ComPtr<ID3D11Resource>, 4> outputResources;
    ComPtr<ID3D11DepthStencilView> depth;
};

std::array<ComPtr<ID3D11Resource>, 2> g_svoEyeVelocityResources;
std::array<SvoEyeHistory, 2> g_svoEyeHistories;
std::uint64_t g_svoHistoryActivationFrame =
    std::numeric_limits<std::uint64_t>::max();
bool g_loggedSeparateSvoHistoryActive = false;

bool ContainsMarker(const void* bytecode, std::size_t bytecodeLength,
                    std::string_view marker)
{
    if (!bytecode || bytecodeLength < marker.size()) {
        return false;
    }
    const auto* begin = static_cast<const std::uint8_t*>(bytecode);
    const auto* end = begin + bytecodeLength;
    return std::search(begin, end, marker.begin(), marker.end()) != end;
}

std::uint32_t Crc32C(const void* data, std::size_t size)
{
    // Castagnoli CRC-32C, using the conventional inverted initial/final state.
    // Special K also identifies shaders with CRC-32C, so this gives us a useful
    // cross-check against the hash displayed in its shader view.
    std::uint32_t crc = 0xFFFFFFFFU;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0x82F63B78U & mask);
        }
    }
    return ~crc;
}

void CaptureCandidate(const void* bytecode, std::size_t bytecodeLength)
{
    const std::uint32_t hash = bytecode && bytecodeLength > 0
                                   ? Crc32C(bytecode, bytecodeLength)
                                   : 0U;
    const bool materialCandidate =
        ContainsMarker(bytecode, bytecodeLength, kMaterialMarker) &&
        ContainsMarker(bytecode, bytecodeLength, kScreenMarker);
    if (!materialCandidate && hash != kSvoTemporalShaderHash &&
        hash != kDoorWavyShaderHash) {
        return;
    }
    std::scoped_lock lock(g_captureMutex);
    if (!g_capturedShaders.insert(hash).second) {
        return;
    }

    std::error_code error;
    const std::filesystem::path directory = ModuleDirectory() / L"KCD1VR-shaders";
    std::filesystem::create_directories(directory, error);
    if (error) {
        Log("Shader capture could not create output directory: error=%d",
            error.value());
        return;
    }

    std::ostringstream name;
    name << "ps_" << std::hex << std::setfill('0') << std::setw(8) << hash
         << '_' << std::dec << bytecodeLength << ".dxbc";
    const std::filesystem::path outputPath = directory / name.str();
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    output.write(static_cast<const char*>(bytecode),
                 static_cast<std::streamsize>(bytecodeLength));
    if (!output) {
        Log("Shader capture failed to write %s", outputPath.string().c_str());
        return;
    }
    const char* description = hash == kSvoTemporalShaderHash
                                  ? "marked SVO temporal"
                              : hash == kDoorWavyShaderHash
                                  ? "marked wavy-door"
                                  : "candidate material";
    Log("Captured %s pixel shader: crc32c=%08x, bytes=%zu, file=%s",
        description, hash, bytecodeLength, outputPath.string().c_str());
}

void RetainPixelShaderBytecode(std::uint32_t hash, const void* bytecode,
                               std::size_t bytecodeLength)
{
    if (hash == 0 || !bytecode || bytecodeLength == 0) {
        return;
    }
    std::scoped_lock lock(g_captureMutex);
    if (g_retainedShaderBytecode.contains(hash)) {
        return;
    }
    if (bytecodeLength > kRetainedShaderBytecodeLimit -
                             std::min(g_retainedShaderBytecodeBytes,
                                      kRetainedShaderBytecodeLimit)) {
        if (!g_loggedShaderRetentionLimit.exchange(true)) {
            Log("SHADER BROWSER: bytecode retention reached its 64 MiB safety limit; already-retained shaders can still be marked");
        }
        return;
    }
    auto& retained = g_retainedShaderBytecode[hash];
    retained.assign(static_cast<const std::uint8_t*>(bytecode),
                    static_cast<const std::uint8_t*>(bytecode) + bytecodeLength);
    g_retainedShaderBytecodeBytes += bytecodeLength;
}

bool SaveRetainedPixelShader(std::uint32_t hash)
{
    std::scoped_lock lock(g_captureMutex);
    const auto found = g_retainedShaderBytecode.find(hash);
    if (found == g_retainedShaderBytecode.end()) {
        Log("SHADER BROWSER: bytecode for %08x was not retained", hash);
        return false;
    }

    std::error_code error;
    const std::filesystem::path directory = ModuleDirectory() / L"KCD1VR-shaders";
    std::filesystem::create_directories(directory, error);
    if (error) {
        Log("SHADER BROWSER: could not create bytecode directory: error=%d",
            error.value());
        return false;
    }
    std::ostringstream name;
    name << "ps_" << std::hex << std::setfill('0') << std::setw(8) << hash
         << '_' << std::dec << found->second.size() << ".dxbc";
    const std::filesystem::path outputPath = directory / name.str();
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(found->second.data()),
                 static_cast<std::streamsize>(found->second.size()));
    if (!output) {
        Log("SHADER BROWSER: failed to save bytecode for %08x", hash);
        return false;
    }
    Log("SHADER BROWSER: saved marked shader %08x bytecode to %s", hash,
        outputPath.string().c_str());
    return true;
}

void RetainVertexShaderBytecode(std::uint32_t hash, const void* bytecode,
                                std::size_t bytecodeLength)
{
    if (hash == 0 || !bytecode || bytecodeLength == 0) {
        return;
    }
    std::scoped_lock lock(g_captureMutex);
    if (g_retainedVertexShaderBytecode.contains(hash)) {
        return;
    }
    if (bytecodeLength > kRetainedVertexShaderBytecodeLimit -
                             std::min(g_retainedVertexShaderBytecodeBytes,
                                      kRetainedVertexShaderBytecodeLimit)) {
        Log("DLSS 1TX VERTEX TRACE: bytecode retention reached its 16 MiB safety limit");
        return;
    }
    auto& retained = g_retainedVertexShaderBytecode[hash];
    retained.assign(static_cast<const std::uint8_t*>(bytecode),
                    static_cast<const std::uint8_t*>(bytecode) + bytecodeLength);
    g_retainedVertexShaderBytecodeBytes += bytecodeLength;
}

bool SaveRetainedVertexShader(std::uint32_t hash)
{
    std::scoped_lock lock(g_captureMutex);
    const auto found = g_retainedVertexShaderBytecode.find(hash);
    if (found == g_retainedVertexShaderBytecode.end()) {
        Log("DLSS 1TX VERTEX TRACE: bytecode for %08x was not retained", hash);
        return false;
    }

    std::error_code error;
    const std::filesystem::path directory = ModuleDirectory() / L"KCD1VR-shaders";
    std::filesystem::create_directories(directory, error);
    if (error) {
        Log("DLSS 1TX VERTEX TRACE: could not create bytecode directory: error=%d",
            error.value());
        return false;
    }
    std::ostringstream name;
    name << "vs_" << std::hex << std::setfill('0') << std::setw(8) << hash
         << '_' << std::dec << found->second.size() << ".dxbc";
    const std::filesystem::path outputPath = directory / name.str();
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(found->second.data()),
                 static_cast<std::streamsize>(found->second.size()));
    if (!output) {
        Log("DLSS 1TX VERTEX TRACE: failed to save bytecode for %08x", hash);
        return false;
    }
    Log("DLSS 1TX VERTEX TRACE: saved vertex shader %08x bytecode to %s", hash,
        outputPath.string().c_str());
    return true;
}

bool PatchMotionVectorOutput(std::uint32_t hash, int mode, const void* bytecode,
                             std::size_t bytecodeLength,
                             std::vector<std::uint8_t>& patched)
{
    if (!bytecode || mode < 1 || mode > 2 ||
        bytecodeLength % sizeof(std::uint32_t) != 0) {
        return false;
    }
    patched.assign(static_cast<const std::uint8_t*>(bytecode),
                   static_cast<const std::uint8_t*>(bytecode) + bytecodeLength);
    auto* words = reinterpret_cast<std::uint32_t*>(patched.data());
    const std::size_t wordCount = patched.size() / sizeof(std::uint32_t);
    std::size_t matches = 0;
    for (std::size_t i = 0; i + 8 < wordCount; ++i) {
        // Unique tail of the shader's final motion-vector MAD:
        // mad o3.xy, r0.xy, l(0.5,0.5,0,0), l(0.498039,0.498039,0,0)
        if (words[i] == 0x3F000000U && words[i + 1] == 0x3F000000U &&
            words[i + 2] == 0U && words[i + 3] == 0U &&
            words[i + 4] == 0x00004002U &&
            words[i + 5] == 0x3EFEFEFFU &&
            words[i + 6] == 0x3EFEFEFFU &&
            words[i + 7] == 0U && words[i + 8] == 0U) {
            words[i] = 0U;
            words[i + 1] = 0U;
            if (mode == 2) {
                // Maximum packed velocity makes temporal consumers reject
                // history for these pixels instead of smearing stale colour.
                words[i + 5] = 0x3F800000U;
                words[i + 6] = 0x3F800000U;
            }
            ++matches;
        }
    }
    if (matches != 1 || !UpdateDxbcChecksum(patched.data(), patched.size())) {
        Log("Refused shader %08x motion patch: expected one bytecode pattern, found %zu",
            hash, matches);
        patched.clear();
        return false;
    }
    return true;
}

bool BypassSvoTemporalHistory(const void* bytecode, std::size_t bytecodeLength,
                              std::vector<std::uint8_t>& patched)
{
    constexpr std::size_t firstBlendConstant = 1740;
    constexpr std::size_t secondBlendConstant = 1821;
    constexpr std::size_t expectedBytecodeLength = 8432;
    if (!bytecode || bytecodeLength != expectedBytecodeLength ||
        bytecodeLength % sizeof(std::uint32_t) != 0) {
        return false;
    }
    patched.assign(static_cast<const std::uint8_t*>(bytecode),
                   static_cast<const std::uint8_t*>(bytecode) + bytecodeLength);
    auto* words = reinterpret_cast<std::uint32_t*>(patched.data());
    const std::size_t wordCount = patched.size() / sizeof(std::uint32_t);
    if (wordCount <= secondBlendConstant ||
        words[firstBlendConstant] != 0xBE800000U ||
        words[secondBlendConstant] != 0xBE800000U) {
        Log("Refused shader 0025b6ef history bypass: bytecode validation failed");
        patched.clear();
        return false;
    }

    // In both GetBlendFactor copies, change the depth rejection bias from
    // -0.25 to +1.0. The saturating MAD therefore returns 1, and the existing
    // lerps select the newly computed SVO values instead of shared history.
    words[firstBlendConstant] = 0x3F800000U;
    words[secondBlendConstant] = 0x3F800000U;
    if (!UpdateDxbcChecksum(patched.data(), patched.size())) {
        patched.clear();
        return false;
    }
    return true;
}

void RegisterPixelShader(ID3D11PixelShader* shader, std::uint32_t hash)
{
    if (!shader || hash == 0) {
        return;
    }
    std::unique_lock lock(g_shaderMapMutex);
    g_shaderHashes[shader] = hash;
    g_shaderMapGeneration.fetch_add(1, std::memory_order_release);
}

void RegisterVertexShader(ID3D11VertexShader* shader, std::uint32_t hash)
{
    if (!shader || hash == 0) {
        return;
    }
    std::unique_lock lock(g_shaderMapMutex);
    g_vertexShaderHashes[shader] = hash;
}

std::uint32_t FindVertexShaderHash(ID3D11VertexShader* shader)
{
    if (!shader) {
        return 0;
    }
    std::shared_lock lock(g_shaderMapMutex);
    const auto found = g_vertexShaderHashes.find(shader);
    return found != g_vertexShaderHashes.end() ? found->second : 0U;
}

std::uint32_t FindPixelShaderHash(ID3D11PixelShader* shader)
{
    if (!shader) {
        return 0;
    }
    const std::uint64_t generation =
        g_shaderMapGeneration.load(std::memory_order_acquire);
    const std::uintptr_t pointer = reinterpret_cast<std::uintptr_t>(shader);
    PixelShaderCacheEntry& cache =
        t_pixelShaderCache[((pointer >> 4U) ^ (pointer >> 12U)) &
                           (t_pixelShaderCache.size() - 1U)];
    if (shader == cache.shader && generation == cache.generation) {
        return cache.hash;
    }
    std::shared_lock lock(g_shaderMapMutex);
    const auto found = g_shaderHashes.find(shader);
    cache.shader = shader;
    cache.hash = found != g_shaderHashes.end() ? found->second : 0U;
    cache.generation = generation;
    return cache.hash;
}

void ObserveCurrentShader()
{
    if (!g_browserCapturing.load(std::memory_order_relaxed) ||
        t_currentPixelShaderHash == 0) {
        return;
    }
    const std::uint32_t generation =
        g_captureGeneration.load(std::memory_order_relaxed);
    if (t_seenCaptureGeneration != generation) {
        t_seenCaptureGeneration = generation;
        t_lastObservedShaderHash = 0;
    }
    if (t_lastObservedShaderHash == t_currentPixelShaderHash) {
        return;
    }
    t_lastObservedShaderHash = t_currentPixelShaderHash;
    std::scoped_lock lock(g_browserMutex);
    g_observedShaders.insert(t_currentPixelShaderHash);
}

std::string DescribeTextureView(ID3D11View* view)
{
    if (!view) {
        return "null";
    }
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    if (!resource) {
        return "no-resource";
    }
    ID3D11Texture2D* texture = nullptr;
    std::ostringstream description;
    description << resource;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        description << ':' << desc.Width << 'x' << desc.Height
                    << ":fmt" << static_cast<unsigned>(desc.Format)
                    << ":a" << desc.ArraySize << ":m" << desc.MipLevels;
        texture->Release();
    } else {
        description << ":non2d";
    }
    resource->Release();
    return description.str();
}

void TraceSvoDraw(ID3D11DeviceContext* context)
{
    if (!context || !GetConfig().traceSvoResources ||
        t_currentPixelShaderHash != kSvoTemporalShaderHash) {
        return;
    }
    const std::uint32_t traceIndex = g_svoTraceCount.fetch_add(1);
    if (traceIndex >= 12) {
        return;
    }

    UINT viewportCount = 8;
    D3D11_VIEWPORT viewports[8]{};
    context->RSGetViewports(&viewportCount, viewports);
    std::ostringstream header;
    header << "SVO TRACE " << (traceIndex + 1) << "/12 frame="
           << g_presentCounter.load(std::memory_order_relaxed)
           << " context=" << static_cast<unsigned>(context->GetType())
           << " viewports=" << viewportCount;
    if (viewportCount > 0) {
        header << " vp0=" << viewports[0].TopLeftX << ',' << viewports[0].TopLeftY
               << ' ' << viewports[0].Width << 'x' << viewports[0].Height
               << " z=" << viewports[0].MinDepth << '-' << viewports[0].MaxDepth;
    }
    Log("%s", header.str().c_str());

    ID3D11RenderTargetView* renderTargets[4]{};
    context->OMGetRenderTargets(4, renderTargets, nullptr);
    std::ostringstream outputs;
    outputs << "SVO TRACE outputs";
    for (unsigned slot = 0; slot < 4; ++slot) {
        outputs << " r" << slot << '=' << DescribeTextureView(renderTargets[slot]);
        if (renderTargets[slot]) {
            renderTargets[slot]->Release();
        }
    }
    Log("%s", outputs.str().c_str());

    ID3D11ShaderResourceView* resources[15]{};
    context->PSGetShaderResources(0, 15, resources);
    constexpr unsigned interestingSlots[]{0, 1, 2, 3, 4, 8, 10, 11, 14};
    std::ostringstream inputs;
    inputs << "SVO TRACE inputs";
    for (const unsigned slot : interestingSlots) {
        inputs << " t" << slot << '=' << DescribeTextureView(resources[slot]);
    }
    for (ID3D11ShaderResourceView* resource : resources) {
        if (resource) {
            resource->Release();
        }
    }
    Log("%s", inputs.str().c_str());
}

void TraceDoorShaderDraw(ID3D11DeviceContext* context)
{
    if (!context || !GetConfig().traceDoorShaderResources ||
        t_currentPixelShaderHash != kDoorWavyShaderHash) {
        return;
    }
    const std::uint32_t traceIndex = g_doorShaderTraceCount.fetch_add(1);
    if (traceIndex >= 12) {
        return;
    }

    UINT viewportCount = 4;
    D3D11_VIEWPORT viewports[4]{};
    context->RSGetViewports(&viewportCount, viewports);
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology(&topology);
    std::ostringstream header;
    header << "DOOR SHADER TRACE " << (traceIndex + 1) << "/12 frame="
           << g_presentCounter.load(std::memory_order_relaxed)
           << " context=" << static_cast<unsigned>(context->GetType())
           << " topology=" << static_cast<unsigned>(topology)
           << " viewports=" << viewportCount;
    if (viewportCount > 0) {
        header << " vp0=" << viewports[0].TopLeftX << ',' << viewports[0].TopLeftY
               << ' ' << viewports[0].Width << 'x' << viewports[0].Height;
    }
    Log("%s", header.str().c_str());

    ID3D11RenderTargetView* renderTargets[8]{};
    ID3D11DepthStencilView* depth = nullptr;
    context->OMGetRenderTargets(8, renderTargets, &depth);
    std::ostringstream outputs;
    outputs << "DOOR SHADER TRACE outputs";
    for (unsigned slot = 0; slot < 8; ++slot) {
        if (renderTargets[slot]) {
            outputs << " r" << slot << '=' << DescribeTextureView(renderTargets[slot]);
            renderTargets[slot]->Release();
        }
    }
    outputs << " depth=" << DescribeTextureView(depth);
    if (depth) {
        depth->Release();
    }
    Log("%s", outputs.str().c_str());

    ID3D11ShaderResourceView* resources[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    context->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,
                                  resources);
    std::ostringstream inputs;
    inputs << "DOOR SHADER TRACE inputs";
    for (unsigned slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
         ++slot) {
        if (resources[slot]) {
            inputs << " t" << slot << '=' << DescribeTextureView(resources[slot]);
            resources[slot]->Release();
        }
    }
    Log("%s", inputs.str().c_str());
}

void TraceDlss1TxCandidate(ID3D11DeviceContext* context)
{
    const std::uint64_t present =
        g_presentCounter.load(std::memory_order_relaxed);
    const std::uint64_t firstPresent =
        g_dlss1TxTraceFirstPresent.load(std::memory_order_relaxed);
    if (!context || !GetConfig().enableDlss || t_currentPixelShaderHash == 0 ||
        t_currentPixelShaderIsDlssPostAa ||
        g_dlss1TxTraceCount.load(std::memory_order_relaxed) >= 32 ||
        (firstPresent != std::numeric_limits<std::uint64_t>::max() &&
         present > firstPresent + 120)) {
        return;
    }

    ComPtr<ID3D11RenderTargetView> renderTarget;
    context->OMGetRenderTargets(1, &renderTarget, nullptr);
    if (!renderTarget) {
        return;
    }
    ComPtr<ID3D11Resource> targetResource;
    renderTarget->GetResource(&targetResource);
    ComPtr<ID3D11Texture2D> targetTexture;
    if (FAILED(targetResource.As(&targetTexture))) {
        return;
    }
    D3D11_TEXTURE2D_DESC targetDescription{};
    targetTexture->GetDesc(&targetDescription);
    // KCD's per-eye PostAA targets are portrait, full-resolution textures.
    // This excludes the landscape menus and side-by-side backbuffer while
    // retaining the small set of plausible 1TX fullscreen passes.
    if (targetDescription.Height < 1000 ||
        targetDescription.Height <= targetDescription.Width) {
        return;
    }
    std::uint64_t unset = std::numeric_limits<std::uint64_t>::max();
    g_dlss1TxTraceFirstPresent.compare_exchange_strong(
        unset, present, std::memory_order_relaxed);

    ComPtr<ID3D11ShaderResourceView> color;
    context->PSGetShaderResources(0, 1, &color);
    if (!color) {
        return;
    }
    ComPtr<ID3D11Resource> colorResource;
    color->GetResource(&colorResource);
    ComPtr<ID3D11Texture2D> colorTexture;
    if (FAILED(colorResource.As(&colorTexture))) {
        return;
    }
    D3D11_TEXTURE2D_DESC colorDescription{};
    colorTexture->GetDesc(&colorDescription);
    if (colorDescription.Width != targetDescription.Width ||
        colorDescription.Height != targetDescription.Height) {
        return;
    }

    {
        std::scoped_lock lock(g_dlss1TxTraceMutex);
        if (!g_dlss1TxTracedShaders.insert(t_currentPixelShaderHash).second) {
            return;
        }
    }
    const std::uint32_t traceIndex =
        g_dlss1TxTraceCount.fetch_add(1, std::memory_order_relaxed);
    if (traceIndex >= 32) {
        return;
    }

    ID3D11ShaderResourceView* resources[17]{};
    context->PSGetShaderResources(0, 17, resources);
    std::ostringstream inputs;
    inputs << "DLSS 1TX CANDIDATE " << (traceIndex + 1) << "/32 shader="
           << std::hex << std::setfill('0') << std::setw(8)
           << t_currentPixelShaderHash << std::dec << " target="
           << targetDescription.Width << 'x' << targetDescription.Height
           << ":fmt" << static_cast<unsigned>(targetDescription.Format)
           << " inputs";
    for (unsigned slot = 0; slot < 17; ++slot) {
        if (resources[slot]) {
            inputs << " t" << slot << '=' << DescribeTextureView(resources[slot]);
            resources[slot]->Release();
        }
    }
    Log("%s", inputs.str().c_str());
    SaveRetainedPixelShader(t_currentPixelShaderHash);

    if (t_currentPixelShaderHash == kDlss1TxResolveShaderHash) {
        ID3D11VertexShader* rawVertexShader = nullptr;
        context->VSGetShader(&rawVertexShader, nullptr, nullptr);
        ComPtr<ID3D11VertexShader> vertexShader;
        vertexShader.Attach(rawVertexShader);
        const std::uint32_t vertexHash = FindVertexShaderHash(vertexShader.Get());
        if (vertexHash != 0) {
            Log("DLSS 1TX VERTEX TRACE: resolve pixel shader cb02cdb7 uses vertex shader %08x",
                vertexHash);
            SaveRetainedVertexShader(vertexHash);
        } else {
            Log("DLSS 1TX VERTEX TRACE: resolve pixel shader cb02cdb7 vertex shader was not registered");
        }
    }
}

void ResetSvoHistory(const char* reason)
{
    for (auto& resource : g_svoEyeVelocityResources) {
        resource.Reset();
    }
    for (auto& history : g_svoEyeHistories) {
        history = {};
    }
    g_svoHistoryActivationFrame = std::numeric_limits<std::uint64_t>::max();
    g_loggedSeparateSvoHistoryActive = false;
    if (reason) {
        Log("SVO HISTORY: reset per-eye resources (%s)", reason);
    }
}

int IdentifySvoEye(ID3D11DeviceContext* context)
{
    ID3D11ShaderResourceView* rawVelocityView = nullptr;
    context->PSGetShaderResources(8, 1, &rawVelocityView);
    ComPtr<ID3D11ShaderResourceView> velocityView;
    velocityView.Attach(rawVelocityView);
    if (!velocityView) {
        return -1;
    }

    ID3D11Resource* rawVelocityResource = nullptr;
    velocityView->GetResource(&rawVelocityResource);
    ComPtr<ID3D11Resource> velocityResource;
    velocityResource.Attach(rawVelocityResource);
    if (!velocityResource) {
        return -1;
    }

    for (unsigned eye = 0; eye < g_svoEyeVelocityResources.size(); ++eye) {
        if (g_svoEyeVelocityResources[eye].Get() == velocityResource.Get()) {
            return static_cast<int>(eye);
        }
    }

    if (!g_svoEyeVelocityResources[0]) {
        g_svoEyeVelocityResources[0] = velocityResource;
        Log("SVO HISTORY: identified first eye from velocity resource %p",
            velocityResource.Get());
        return 0;
    }
    if (!g_svoEyeVelocityResources[1]) {
        g_svoEyeVelocityResources[1] = velocityResource;
        g_svoHistoryActivationFrame =
            g_presentCounter.load(std::memory_order_relaxed) + 1;
        Log("SVO HISTORY: identified second eye from velocity resource %p; separation begins next presented frame",
            velocityResource.Get());
        return 1;
    }

    // Graphics-preset and resolution changes recreate these render resources.
    // Rediscover both eyes instead of assigning a stale eye index.
    ResetSvoHistory("velocity resources changed");
    g_svoEyeVelocityResources[0] = velocityResource;
    Log("SVO HISTORY: identified first eye from new velocity resource %p",
        velocityResource.Get());
    return 0;
}

bool TextureDescriptionsMatch(ID3D11Texture2D* first, ID3D11Texture2D* second)
{
    if (!first || !second) {
        return false;
    }
    D3D11_TEXTURE2D_DESC a{};
    D3D11_TEXTURE2D_DESC b{};
    first->GetDesc(&a);
    second->GetDesc(&b);
    return a.Width == b.Width && a.Height == b.Height &&
           a.MipLevels == b.MipLevels && a.ArraySize == b.ArraySize &&
           a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality;
}

bool EnsureSvoEyeHistory(ID3D11DeviceContext* context, unsigned eye,
                         SvoHistoryOverride& state)
{
    if (eye >= g_svoEyeHistories.size()) {
        return false;
    }

    ID3D11Device* rawDevice = nullptr;
    context->GetDevice(&rawDevice);
    ComPtr<ID3D11Device> device;
    device.Attach(rawDevice);
    if (!device) {
        return false;
    }

    std::array<ComPtr<ID3D11Texture2D>, 4> outputTextures;
    for (unsigned slot = 0; slot < outputTextures.size(); ++slot) {
        if (!state.outputResources[slot] ||
            FAILED(state.outputResources[slot].As(&outputTextures[slot]))) {
            return false;
        }
    }

    SvoEyeHistory& history = g_svoEyeHistories[eye];
    bool reusable = history.device.Get() == device.Get();
    for (unsigned slot = 0; reusable && slot < history.textures.size(); ++slot) {
        reusable = history.views[slot] &&
                   TextureDescriptionsMatch(history.textures[slot].Get(),
                                            outputTextures[slot].Get());
    }
    if (reusable) {
        return true;
    }

    history = {};
    history.device = device;
    for (unsigned slot = 0; slot < history.textures.size(); ++slot) {
        D3D11_TEXTURE2D_DESC desc{};
        outputTextures[slot]->GetDesc(&desc);
        if (desc.MipLevels != 1 || desc.ArraySize != 1 ||
            desc.SampleDesc.Count != 1 ||
            desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
            Log("SVO HISTORY: unsupported output r%u (%ux%u format=%u mips=%u array=%u samples=%u)",
                slot, desc.Width, desc.Height, static_cast<unsigned>(desc.Format),
                desc.MipLevels, desc.ArraySize, desc.SampleDesc.Count);
            history = {};
            return false;
        }

        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        constexpr UINT bytesPerPixel = 8;
        std::vector<std::uint8_t> zeros(
            static_cast<std::size_t>(desc.Width) * desc.Height * bytesPerPixel);
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = zeros.data();
        initial.SysMemPitch = desc.Width * bytesPerPixel;
        initial.SysMemSlicePitch = initial.SysMemPitch * desc.Height;
        HRESULT result = device->CreateTexture2D(
            &desc, &initial, history.textures[slot].GetAddressOf());
        if (FAILED(result)) {
            Log("SVO HISTORY: CreateTexture2D failed for eye %u r%u (HRESULT=0x%08X)",
                eye, slot, static_cast<unsigned>(result));
            history = {};
            return false;
        }
        result = device->CreateShaderResourceView(
            history.textures[slot].Get(), nullptr, history.views[slot].GetAddressOf());
        if (FAILED(result)) {
            Log("SVO HISTORY: CreateShaderResourceView failed for eye %u r%u (HRESULT=0x%08X)",
                eye, slot, static_cast<unsigned>(result));
            history = {};
            return false;
        }
        if (slot == 0) {
            Log("SVO HISTORY: created private %ux%u history set for eye %u",
                desc.Width, desc.Height, eye);
        }
    }
    return true;
}

bool BeginSvoHistoryOverride(ID3D11DeviceContext* context,
                             SvoHistoryOverride& state)
{
    if (!context || !GetConfig().separateSvoHistoryPerEye ||
        t_currentPixelShaderHash != kSvoTemporalShaderHash ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return false;
    }

    const int identifiedEye = IdentifySvoEye(context);
    if (identifiedEye < 0 ||
        !g_svoEyeVelocityResources[0] || !g_svoEyeVelocityResources[1] ||
        g_presentCounter.load(std::memory_order_relaxed) <
            g_svoHistoryActivationFrame) {
        return false;
    }
    state.eye = static_cast<unsigned>(identifiedEye);

    ID3D11RenderTargetView* rawOutputs[4]{};
    ID3D11DepthStencilView* rawDepth = nullptr;
    context->OMGetRenderTargets(4, rawOutputs, &rawDepth);
    state.depth.Attach(rawDepth);
    for (unsigned slot = 0; slot < state.outputs.size(); ++slot) {
        state.outputs[slot].Attach(rawOutputs[slot]);
        if (!state.outputs[slot]) {
            return false;
        }
        ID3D11Resource* rawResource = nullptr;
        state.outputs[slot]->GetResource(&rawResource);
        state.outputResources[slot].Attach(rawResource);
        if (!state.outputResources[slot]) {
            return false;
        }
    }

    ID3D11ShaderResourceView* rawInputs[4]{};
    context->PSGetShaderResources(0, 4, rawInputs);
    for (unsigned slot = 0; slot < state.originalInputs.size(); ++slot) {
        state.originalInputs[slot].Attach(rawInputs[slot]);
    }

    if (!EnsureSvoEyeHistory(context, state.eye, state)) {
        return false;
    }

    ID3D11ShaderResourceView* privateInputs[4]{};
    for (unsigned slot = 0; slot < g_svoEyeHistories[state.eye].views.size(); ++slot) {
        privateInputs[slot] = g_svoEyeHistories[state.eye].views[slot].Get();
    }
    context->PSSetShaderResources(0, 4, privateInputs);
    state.active = true;
    if (!g_loggedSeparateSvoHistoryActive) {
        g_loggedSeparateSvoHistoryActive = true;
        Log("SVO HISTORY: independent per-eye temporal history is active");
    }
    return true;
}

void EndSvoHistoryOverride(ID3D11DeviceContext* context,
                           SvoHistoryOverride& state)
{
    if (!state.active || !context) {
        return;
    }

    ID3D11ShaderResourceView* nullInputs[4]{};
    context->PSSetShaderResources(0, 4, nullInputs);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    for (unsigned slot = 0; slot < state.outputResources.size(); ++slot) {
        context->CopyResource(g_svoEyeHistories[state.eye].textures[slot].Get(),
                              state.outputResources[slot].Get());
    }

    ID3D11RenderTargetView* rawOutputs[4]{};
    ID3D11ShaderResourceView* rawInputs[4]{};
    for (unsigned slot = 0; slot < state.outputs.size(); ++slot) {
        rawOutputs[slot] = state.outputs[slot].Get();
        rawInputs[slot] = state.originalInputs[slot].Get();
    }
    context->OMSetRenderTargets(4, rawOutputs, state.depth.Get());
    context->PSSetShaderResources(0, 4, rawInputs);
}

template <typename DrawCall>
void ExecuteTrackedDraw(ID3D11DeviceContext* context, DrawCall&& drawCall)
{
    const Config& config = GetConfig();
    const std::uint32_t selected =
        g_selectedSkipHash.load(std::memory_order_relaxed);
    if (selected != 0 && selected == t_currentPixelShaderHash) {
        return;
    }

    const bool browserCapturing =
        g_browserCapturing.load(std::memory_order_relaxed);
    const bool isSvoShader =
        t_currentPixelShaderHash == kSvoTemporalShaderHash;
    const bool traceSvo = config.traceSvoResources && isSvoShader;
    const bool traceDoor = config.traceDoorShaderResources &&
                           t_currentPixelShaderHash == kDoorWavyShaderHash;

    // Almost every game draw takes this path. Avoid constructing thirteen
    // ComPtrs and probing D3D state unless this is the one shader whose history
    // we override, or an explicitly enabled browser/trace operation needs it.
    const auto executeAndCaptureDlss = [&] {
        if (t_currentPixelShaderIsDlssPostAa &&
            config.dlssReplaceNativeTemporalAa) {
            if (ID3D11PixelShader* combinedShader =
                    BeginDlss1TxMotionReplay(context, t_currentPixelShader)) {
                g_originalPSSetShader(context, combinedShader, nullptr, 0);
                drawCall();
                g_originalPSSetShader(context, t_currentPixelShader, nullptr, 0);
                EndDlss1TxMotionReplay(context);
                CaptureDlssPostAaInputs(context, t_currentPixelShader);
                return;
            }
        }
        drawCall();
        if (config.enableDlss && config.dlssUseRawPreSmaaColor &&
            t_currentPixelShaderHash == kDlssPreSmaaColorShaderHash) {
            CaptureDlssPreSmaaColor(context);
        }
        if (t_currentPixelShaderIsDlssPostAa) {
            if (ID3D11PixelShader* motionShader =
                    BeginDlss1TxMotionReplay(context, t_currentPixelShader)) {
                g_originalPSSetShader(context, motionShader, nullptr, 0);
                drawCall();
                g_originalPSSetShader(context, t_currentPixelShader, nullptr, 0);
                EndDlss1TxMotionReplay(context);
            }
            CaptureDlssPostAaInputs(context, t_currentPixelShader);
        }
    };

    if (!browserCapturing && !traceSvo && !traceDoor &&
        !(config.separateSvoHistoryPerEye && isSvoShader)) {
        executeAndCaptureDlss();
        return;
    }

    if (traceSvo) {
        TraceSvoDraw(context);
    }
    if (traceDoor) {
        TraceDoorShaderDraw(context);
    }
    if (!(config.separateSvoHistoryPerEye && isSvoShader)) {
        executeAndCaptureDlss();
        return;
    }

    SvoHistoryOverride state;
    BeginSvoHistoryOverride(context, state);
    executeAndCaptureDlss();
    EndSvoHistoryOverride(context, state);
}

void STDMETHODCALLTYPE HookPSSetShader(
    ID3D11DeviceContext* context, ID3D11PixelShader* shader,
    ID3D11ClassInstance* const* classInstances, UINT classInstanceCount)
{
    t_currentPixelShader = shader;
    t_currentPixelShaderIsDlssPostAa = IsDlssPostAaShader(shader);
    t_currentPixelShaderHash = FindPixelShaderHash(shader);
    ObserveCurrentShader();
    g_originalPSSetShader(context, shader, classInstances, classInstanceCount);
}

void STDMETHODCALLTYPE HookPSSetShaderDeferred(
    ID3D11DeviceContext* context, ID3D11PixelShader* shader,
    ID3D11ClassInstance* const* classInstances, UINT classInstanceCount)
{
    t_currentPixelShader = shader;
    t_currentPixelShaderIsDlssPostAa = IsDlssPostAaShader(shader);
    t_currentPixelShaderHash = FindPixelShaderHash(shader);
    ObserveCurrentShader();
    g_originalPSSetShaderDeferred(context, shader, classInstances, classInstanceCount);
}

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* context, UINT indexCount,
                                       UINT startIndex, INT baseVertex)
{
    ExecuteTrackedDraw(context, [&] {
        g_originalDrawIndexed(context, indexCount, startIndex, baseVertex);
    });
}

void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* context, UINT vertexCount,
                                UINT startVertex)
{
    ExecuteTrackedDraw(context, [&] {
        g_originalDraw(context, vertexCount, startVertex);
    });
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(
    ID3D11DeviceContext* context, UINT indexCountPerInstance, UINT instanceCount,
    UINT startIndex, INT baseVertex, UINT startInstance)
{
    ExecuteTrackedDraw(context, [&] {
        g_originalDrawIndexedInstanced(context, indexCountPerInstance, instanceCount,
                                       startIndex, baseVertex, startInstance);
    });
}

void STDMETHODCALLTYPE HookDrawInstanced(
    ID3D11DeviceContext* context, UINT vertexCountPerInstance, UINT instanceCount,
    UINT startVertex, UINT startInstance)
{
    ExecuteTrackedDraw(context, [&] {
        g_originalDrawInstanced(context, vertexCountPerInstance, instanceCount,
                                startVertex, startInstance);
    });
}

void STDMETHODCALLTYPE HookDrawAuto(ID3D11DeviceContext* context)
{
    ExecuteTrackedDraw(context, [&] {
        g_originalDrawAuto(context);
    });
}

void STDMETHODCALLTYPE HookDrawIndexedInstancedIndirect(
    ID3D11DeviceContext* context, ID3D11Buffer* argumentBuffer, UINT alignedOffset)
{
    ExecuteTrackedDraw(context, [&] {
        g_originalDrawIndexedInstancedIndirect(context, argumentBuffer, alignedOffset);
    });
}

void STDMETHODCALLTYPE HookDrawInstancedIndirect(
    ID3D11DeviceContext* context, ID3D11Buffer* argumentBuffer, UINT alignedOffset)
{
    ExecuteTrackedDraw(context, [&] {
        g_originalDrawInstancedIndirect(context, argumentBuffer, alignedOffset);
    });
}

void STDMETHODCALLTYPE HookDrawIndexedInstancedIndirectDeferred(
    ID3D11DeviceContext* context, ID3D11Buffer* argumentBuffer, UINT alignedOffset)
{
    ExecuteTrackedDraw(context, [&] {
        g_originalDrawIndexedInstancedIndirectDeferred(context, argumentBuffer,
                                                       alignedOffset);
    });
}

void STDMETHODCALLTYPE HookDrawInstancedIndirectDeferred(
    ID3D11DeviceContext* context, ID3D11Buffer* argumentBuffer, UINT alignedOffset)
{
    ExecuteTrackedDraw(context, [&] {
        g_originalDrawInstancedIndirectDeferred(context, argumentBuffer, alignedOffset);
    });
}

HRESULT STDMETHODCALLTYPE HookCreatePixelShader(
    ID3D11Device* device, const void* bytecode, SIZE_T bytecodeLength,
    ID3D11ClassLinkage* classLinkage, ID3D11PixelShader** pixelShader)
{
    CaptureCandidate(bytecode, bytecodeLength);
    const std::uint32_t hash = bytecode && bytecodeLength > 0
                                   ? Crc32C(bytecode, bytecodeLength)
                                   : 0U;
    RetainPixelShaderBytecode(hash, bytecode, bytecodeLength);
    if (hash == kSvoTemporalShaderHash && GetConfig().bypassSvoTemporalHistory) {
        std::vector<std::uint8_t> patched;
        if (BypassSvoTemporalHistory(bytecode, bytecodeLength, patched)) {
            const HRESULT patchedResult = g_originalCreatePixelShader(
                device, patched.data(), patched.size(), classLinkage, pixelShader);
            if (SUCCEEDED(patchedResult)) {
                if (pixelShader) {
                    RegisterPixelShader(*pixelShader, hash);
                }
                if (!g_loggedSvoHistoryPatch.exchange(true)) {
                    Log("Shader 0025b6ef created with previous-frame SVO history bypassed; current-frame SVO lighting is preserved");
                }
                return patchedResult;
            }
            Log("Patched shader 0025b6ef was rejected (HRESULT=0x%08X); using original bytecode",
                static_cast<unsigned>(patchedResult));
        }
    }
    if (hash == kDoorWavyShaderHash && GetConfig().doorMotionMode != 0) {
        std::vector<std::uint8_t> patched;
        const int motionMode = GetConfig().doorMotionMode;
        if (PatchMotionVectorOutput(hash, motionMode, bytecode, bytecodeLength,
                                    patched)) {
            const HRESULT patchedResult = g_originalCreatePixelShader(
                device, patched.data(), patched.size(), classLinkage, pixelShader);
            if (SUCCEEDED(patchedResult)) {
                if (pixelShader) {
                    RegisterPixelShader(*pixelShader, hash);
                }
                if (!g_loggedDoorMotionPatch.exchange(true)) {
                    Log("Shader bff18ada motion mode %d active; door material, depth and colour outputs are unchanged",
                        motionMode);
                }
                return patchedResult;
            }
            Log("Patched shader bff18ada was rejected (HRESULT=0x%08X); using original bytecode",
                static_cast<unsigned>(patchedResult));
        }
    }
    if (hash == kTargetShaderHash && GetConfig().neutralizeShader1919AMotion) {
        std::vector<std::uint8_t> patched;
        if (PatchMotionVectorOutput(hash, 1, bytecode, bytecodeLength, patched)) {
            const HRESULT patchedResult = g_originalCreatePixelShader(
                device, patched.data(), patched.size(), classLinkage, pixelShader);
            if (SUCCEEDED(patchedResult)) {
                if (pixelShader) {
                    RegisterPixelShader(*pixelShader, hash);
                }
                if (!g_loggedMotionPatch.exchange(true)) {
                    Log("Shader 0001919a created with neutral motion-vector output; material outputs are unchanged");
                }
                return patchedResult;
            }
            Log("Patched shader 0001919a was rejected (HRESULT=0x%08X); using original bytecode",
                static_cast<unsigned>(patchedResult));
        }
    }
    const HRESULT result = g_originalCreatePixelShader(
        device, bytecode, bytecodeLength, classLinkage, pixelShader);
    if (SUCCEEDED(result) && pixelShader) {
        RegisterPixelShader(*pixelShader, hash);
        RegisterDlssPostAaShader(*pixelShader, bytecode, bytecodeLength);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateVertexShader(
    ID3D11Device* device, const void* bytecode, SIZE_T bytecodeLength,
    ID3D11ClassLinkage* classLinkage, ID3D11VertexShader** vertexShader)
{
    const std::uint32_t hash = bytecode && bytecodeLength > 0
                                   ? Crc32C(bytecode, bytecodeLength)
                                   : 0U;
    RetainVertexShaderBytecode(hash, bytecode, bytecodeLength);
    const HRESULT result = g_originalCreateVertexShader(
        device, bytecode, bytecodeLength, classLinkage, vertexShader);
    if (SUCCEEDED(result) && vertexShader) {
        RegisterVertexShader(*vertexShader, hash);
    }
    return result;
}

bool HookContextFunction(void* target, void* replacement, void** original,
                         const char* name)
{
    const MH_STATUS createStatus = MH_CreateHook(target, replacement, original);
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
        Log("MinHook failed to create %s hook: %s", name,
            MH_StatusToString(createStatus));
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
        Log("MinHook failed to enable %s hook: %s", name,
            MH_StatusToString(enableStatus));
        return false;
    }
    return true;
}

bool InstallContextHooks(ID3D11Device* device)
{
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) {
        Log("Could not obtain the D3D11 immediate context for shader browser");
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(context);
    bool installed = true;
    installed &= HookContextFunction(vtable[9], reinterpret_cast<void*>(&HookPSSetShader),
        reinterpret_cast<void**>(&g_originalPSSetShader), "ID3D11DeviceContext::PSSetShader");
    installed &= HookContextFunction(vtable[12], reinterpret_cast<void*>(&HookDrawIndexed),
        reinterpret_cast<void**>(&g_originalDrawIndexed), "ID3D11DeviceContext::DrawIndexed");
    installed &= HookContextFunction(vtable[13], reinterpret_cast<void*>(&HookDraw),
        reinterpret_cast<void**>(&g_originalDraw), "ID3D11DeviceContext::Draw");
    installed &= HookContextFunction(vtable[20], reinterpret_cast<void*>(&HookDrawIndexedInstanced),
        reinterpret_cast<void**>(&g_originalDrawIndexedInstanced), "ID3D11DeviceContext::DrawIndexedInstanced");
    installed &= HookContextFunction(vtable[21], reinterpret_cast<void*>(&HookDrawInstanced),
        reinterpret_cast<void**>(&g_originalDrawInstanced), "ID3D11DeviceContext::DrawInstanced");
    installed &= HookContextFunction(vtable[38], reinterpret_cast<void*>(&HookDrawAuto),
        reinterpret_cast<void**>(&g_originalDrawAuto), "ID3D11DeviceContext::DrawAuto");
    installed &= HookContextFunction(vtable[39], reinterpret_cast<void*>(&HookDrawIndexedInstancedIndirect),
        reinterpret_cast<void**>(&g_originalDrawIndexedInstancedIndirect), "ID3D11DeviceContext::DrawIndexedInstancedIndirect");
    installed &= HookContextFunction(vtable[40], reinterpret_cast<void*>(&HookDrawInstancedIndirect),
        reinterpret_cast<void**>(&g_originalDrawInstancedIndirect), "ID3D11DeviceContext::DrawInstancedIndirect");

    ID3D11DeviceContext* deferred = nullptr;
    if (SUCCEEDED(device->CreateDeferredContext(0, &deferred)) && deferred) {
        void** deferredVtable = *reinterpret_cast<void***>(deferred);
        if (deferredVtable[9] != vtable[9]) {
            installed &= HookContextFunction(deferredVtable[9],
                reinterpret_cast<void*>(&HookPSSetShaderDeferred),
                reinterpret_cast<void**>(&g_originalPSSetShaderDeferred),
                "deferred ID3D11DeviceContext::PSSetShader");
        } else {
            g_originalPSSetShaderDeferred = g_originalPSSetShader;
        }
        if (deferredVtable[39] != vtable[39]) {
            installed &= HookContextFunction(deferredVtable[39],
                reinterpret_cast<void*>(&HookDrawIndexedInstancedIndirectDeferred),
                reinterpret_cast<void**>(&g_originalDrawIndexedInstancedIndirectDeferred),
                "deferred ID3D11DeviceContext::DrawIndexedInstancedIndirect");
        } else {
            g_originalDrawIndexedInstancedIndirectDeferred =
                g_originalDrawIndexedInstancedIndirect;
        }
        if (deferredVtable[40] != vtable[40]) {
            installed &= HookContextFunction(deferredVtable[40],
                reinterpret_cast<void*>(&HookDrawInstancedIndirectDeferred),
                reinterpret_cast<void**>(&g_originalDrawInstancedIndirectDeferred),
                "deferred ID3D11DeviceContext::DrawInstancedIndirect");
        } else {
            g_originalDrawInstancedIndirectDeferred = g_originalDrawInstancedIndirect;
        }
        deferred->Release();
    } else {
        Log("Could not create a D3D11 deferred context for shader-browser hooks");
        installed = false;
    }
    context->Release();
    if (installed) {
        Log("Installed VR-safe pixel-shader browser hooks (F6-F10)");
    }
    return installed;
}

void StartBrowserCapture()
{
    {
        std::scoped_lock lock(g_browserMutex);
        g_observedShaders.clear();
        g_browserShaders.clear();
        g_browserIndex = 0;
    }
    g_selectedSkipHash.store(0, std::memory_order_relaxed);
    g_captureGeneration.fetch_add(1, std::memory_order_relaxed);
    g_captureFramesRemaining = 48;
    g_browserCapturing.store(true, std::memory_order_release);
    Log("SHADER BROWSER: capture started; keep the flickering object visible for about one second");
    MessageBeep(MB_ICONASTERISK);
}

void FinishBrowserCapture()
{
    {
        std::scoped_lock lock(g_browserMutex);
        g_browserShaders.assign(g_observedShaders.begin(), g_observedShaders.end());
        std::sort(g_browserShaders.begin(), g_browserShaders.end());
        g_browserIndex = g_browserShaders.size();
    }
    g_browserCapturing.store(false, std::memory_order_release);
    Log("SHADER BROWSER: capture finished with %zu visible pixel shaders; press F7/F8 to select and skip one",
        g_browserShaders.size());
    MessageBeep(MB_OK);
}

void SelectBrowserShader(bool forward)
{
    std::scoped_lock lock(g_browserMutex);
    if (g_browserShaders.empty()) {
        Log("SHADER BROWSER: no captured shaders; look at the problem and press F6 first");
        return;
    }
    if (g_browserIndex >= g_browserShaders.size()) {
        g_browserIndex = forward ? 0 : g_browserShaders.size() - 1;
    } else if (forward) {
        g_browserIndex = (g_browserIndex + 1) % g_browserShaders.size();
    } else {
        g_browserIndex = (g_browserIndex + g_browserShaders.size() - 1) %
                         g_browserShaders.size();
    }
    const std::uint32_t hash = g_browserShaders[g_browserIndex];
    g_selectedSkipHash.store(hash, std::memory_order_release);
    Log("SHADER BROWSER: skipping pixel shader %08x (%zu/%zu)", hash,
        g_browserIndex + 1, g_browserShaders.size());
}

void MarkBrowserShader()
{
    const std::uint32_t hash = g_selectedSkipHash.load(std::memory_order_acquire);
    if (hash == 0) {
        Log("SHADER BROWSER: F9 pressed with no shader selected");
        return;
    }
    Log("SHADER BROWSER MARKED: pixel shader %08x at index %zu of %zu", hash,
        g_browserIndex + 1, g_browserShaders.size());
    SaveRetainedPixelShader(hash);
    MessageBeep(MB_ICONEXCLAMATION);
}

void DisableBrowserSelection()
{
    g_selectedSkipHash.store(0, std::memory_order_release);
    Log("SHADER BROWSER: selection disabled; all draws restored");
    MessageBeep(MB_OK);
}

}  // namespace

bool InstallShaderCaptureHook(ID3D11Device* device)
{
    if (!device) {
        return false;
    }
    if (g_originalCreatePixelShader && g_originalCreateVertexShader) {
        return true;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    g_createVertexShaderTarget = vtable[12];
    g_createPixelShaderTarget = vtable[15];
    const MH_STATUS createVertexStatus =
        MH_CreateHook(g_createVertexShaderTarget,
                      reinterpret_cast<void*>(&HookCreateVertexShader),
                      reinterpret_cast<void**>(&g_originalCreateVertexShader));
    if (createVertexStatus != MH_OK &&
        createVertexStatus != MH_ERROR_ALREADY_CREATED) {
        Log("MinHook failed to create ID3D11Device::CreateVertexShader hook: %s",
            MH_StatusToString(createVertexStatus));
        return false;
    }
    const MH_STATUS createStatus =
        MH_CreateHook(g_createPixelShaderTarget,
                      reinterpret_cast<void*>(&HookCreatePixelShader),
                      reinterpret_cast<void**>(&g_originalCreatePixelShader));
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
        Log("MinHook failed to create ID3D11Device::CreatePixelShader hook: %s",
            MH_StatusToString(createStatus));
        return false;
    }
    const MH_STATUS enableVertexStatus = MH_EnableHook(g_createVertexShaderTarget);
    if (enableVertexStatus != MH_OK && enableVertexStatus != MH_ERROR_ENABLED) {
        Log("MinHook failed to enable ID3D11Device::CreateVertexShader hook: %s",
            MH_StatusToString(enableVertexStatus));
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(g_createPixelShaderTarget);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
        Log("MinHook failed to enable ID3D11Device::CreatePixelShader hook: %s",
            MH_StatusToString(enableStatus));
        return false;
    }
    Log("Installed shader creation hooks: vertex=%p pixel=%p",
        g_createVertexShaderTarget, g_createPixelShaderTarget);
    return InstallContextHooks(device);
}

void OnShaderBrowserPresent()
{
    g_presentCounter.fetch_add(1, std::memory_order_relaxed);
    if ((GetAsyncKeyState(VK_F6) & 1) != 0) {
        StartBrowserCapture();
    }
    if (g_browserCapturing.load(std::memory_order_acquire)) {
        if (--g_captureFramesRemaining <= 0) {
            FinishBrowserCapture();
        }
        return;
    }
    if ((GetAsyncKeyState(VK_F7) & 1) != 0) {
        SelectBrowserShader(true);
    }
    if ((GetAsyncKeyState(VK_F8) & 1) != 0) {
        SelectBrowserShader(false);
    }
    if ((GetAsyncKeyState(VK_F9) & 1) != 0) {
        MarkBrowserShader();
    }
    if ((GetAsyncKeyState(VK_F10) & 1) != 0) {
        DisableBrowserSelection();
    }
}

}  // namespace kcdvr
