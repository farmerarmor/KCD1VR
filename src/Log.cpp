#include "Log.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>

namespace kcdvr {
namespace {

HMODULE g_selfModule = nullptr;
std::mutex g_logMutex;

}  // namespace

void SetSelfModule(HMODULE module)
{
    g_selfModule = module;
}

std::filesystem::path ModuleDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_selfModule, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

void Log(const char* format, ...)
{
    char message[2048]{};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);

    std::scoped_lock lock(g_logMutex);
    std::ofstream output(ModuleDirectory() / L"KCD1VR.log", std::ios::app);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    output << '[' << now.wHour << ':' << now.wMinute << ':' << now.wSecond << "] "
           << message << '\n';
}

}  // namespace kcdvr

