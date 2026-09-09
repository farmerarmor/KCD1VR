#include "Config.h"
#include "Hooks.h"
#include "Log.h"

#include <Windows.h>
#include <dinput.h>

#include <atomic>

namespace {

HMODULE g_realDInput = nullptr;
std::atomic<bool> g_initialized = false;

FARPROC ResolveRealDInputProc(const char* name)
{
    if (!g_realDInput) {
        wchar_t systemDirectory[MAX_PATH]{};
        GetSystemDirectoryW(systemDirectory, MAX_PATH);
        wchar_t realPath[MAX_PATH]{};
        wsprintfW(realPath, L"%s\\dinput8.dll", systemDirectory);
        g_realDInput = LoadLibraryW(realPath);
    }
    return g_realDInput ? GetProcAddress(g_realDInput, name) : nullptr;
}

DWORD WINAPI InitializeMod(void*)
{
    if (g_initialized.exchange(true)) {
        return 0;
    }
    kcdvr::Log("KCD1VR 0.1.67 flat dialogue screen starting");
    kcdvr::LoadConfig();

    // WHGame is normally mapped before this import proxy runs. Allow unusual
    // loaders a short grace period without doing work under the loader lock.
    for (int attempt = 0; attempt < 100 && !GetModuleHandleW(L"WHGame.dll"); ++attempt) {
        Sleep(50);
    }
    if (!kcdvr::InstallHooks()) {
        kcdvr::Log("One or more hooks could not be installed; see earlier messages");
    }
    return 0;
}

}  // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid,
                                               LPVOID* output, LPUNKNOWN outer)
{
    using Function = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const auto function = reinterpret_cast<Function>(ResolveRealDInputProc("DirectInput8Create"));
    return function ? function(instance, version, iid, output, outer) : E_FAIL;
}

extern "C" HRESULT WINAPI DllCanUnloadNow()
{
    using Function = HRESULT(WINAPI*)();
    const auto function = reinterpret_cast<Function>(ResolveRealDInputProc("DllCanUnloadNow"));
    return function ? function() : S_FALSE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* output)
{
    using Function = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    const auto function = reinterpret_cast<Function>(ResolveRealDInputProc("DllGetClassObject"));
    return function ? function(clsid, iid, output) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI DllRegisterServer()
{
    using Function = HRESULT(WINAPI*)();
    const auto function = reinterpret_cast<Function>(ResolveRealDInputProc("DllRegisterServer"));
    return function ? function() : E_NOTIMPL;
}

extern "C" HRESULT WINAPI DllUnregisterServer()
{
    using Function = HRESULT(WINAPI*)();
    const auto function = reinterpret_cast<Function>(ResolveRealDInputProc("DllUnregisterServer"));
    return function ? function() : E_NOTIMPL;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        kcdvr::SetSelfModule(module);
        const HANDLE thread = CreateThread(nullptr, 0, InitializeMod, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
