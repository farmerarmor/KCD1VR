#pragma once

#include <Windows.h>

#include <filesystem>

namespace kcdvr {

void SetSelfModule(HMODULE module);
std::filesystem::path ModuleDirectory();
void Log(const char* format, ...);

}  // namespace kcdvr

