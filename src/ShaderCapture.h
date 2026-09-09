#pragma once

struct ID3D11Device;

namespace kcdvr {

bool InstallShaderCaptureHook(ID3D11Device* device);
void OnShaderBrowserPresent();

}  // namespace kcdvr
