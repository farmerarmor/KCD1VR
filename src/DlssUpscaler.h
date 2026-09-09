#pragma once

#include <d3d11.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace kcdvr {

struct DlssJitter {
    float x = 0.0F;
    float y = 0.0F;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
};

// Called from the game's stereo camera path and D3D11 shader hooks.
void SetDlssEyeClipPlanes(int eye, float nearPlane, float farPlane);
void SetDlssEyeRenderTargets(ID3D11Texture2D* leftEye,
                             ID3D11Texture2D* rightEye);
void RegisterDlssPostAaShader(ID3D11PixelShader* shader, const void* bytecode,
                              std::size_t bytecodeLength);
bool IsDlssPostAaShader(ID3D11PixelShader* shader);
void CaptureDlssPreSmaaColor(ID3D11DeviceContext* context);
ID3D11PixelShader* BeginDlss1TxMotionReplay(ID3D11DeviceContext* context,
                                            ID3D11PixelShader* shader);
void EndDlss1TxMotionReplay(ID3D11DeviceContext* context);
void CaptureDlssPostAaInputs(ID3D11DeviceContext* context,
                             ID3D11PixelShader* shader);

class DlssUpscaler final {
public:
    static DlssUpscaler& Instance();

    bool Requested() const;
    void Attach(ID3D11Device* device);
    bool Prepare(std::uint32_t sourceWidth, std::uint32_t sourceHeight,
                 std::uint32_t targetWidth, std::uint32_t targetHeight,
                 DXGI_FORMAT outputFormat);
    bool UpscaleEye(std::uint32_t eye);
    bool InputsReady() const;
    ID3D11Texture2D* OutputTexture(std::uint32_t eye) const;
    void BeginFrame();
    void BeginOutputCopyTiming();
    void EndOutputCopyTiming();
    void EndFrameGpuTiming();
    DlssJitter CurrentJitter() const;
    void Shutdown();

private:
    friend void RegisterDlssPostAaShader(ID3D11PixelShader*, const void*,
                                         std::size_t);
    friend void SetDlssEyeClipPlanes(int, float, float);
    friend void SetDlssEyeRenderTargets(ID3D11Texture2D*, ID3D11Texture2D*);
    friend bool IsDlssPostAaShader(ID3D11PixelShader*);
    friend void CaptureDlssPreSmaaColor(ID3D11DeviceContext*);
    friend ID3D11PixelShader* BeginDlss1TxMotionReplay(
        ID3D11DeviceContext*, ID3D11PixelShader*);
    friend void EndDlss1TxMotionReplay(ID3D11DeviceContext*);
    friend void CaptureDlssPostAaInputs(ID3D11DeviceContext*,
                                        ID3D11PixelShader*);
    DlssUpscaler();
    ~DlssUpscaler();
    DlssUpscaler(const DlssUpscaler&) = delete;
    DlssUpscaler& operator=(const DlssUpscaler&) = delete;

    struct Impl;
    Impl* m_impl = nullptr;
};

DXGI_FORMAT DlssOutputFormat(DXGI_FORMAT inputFormat);

}  // namespace kcdvr
