#include "dxgi_proxy.hpp"
#include "swapchain_hooks.hpp"
#include "logging.hpp"
#include <dxgi1_6.h>
#include <iterator>
#include <mutex>
#include <string>

namespace
{
    HMODULE g_systemDxgi{};
    std::once_flag g_once;
    std::once_flag g_configOnce;
    bool g_hooksEnabled{};
    void LoadSystemDxgi() noexcept
    {
        wchar_t systemDirectory[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return;
        std::wstring path(systemDirectory, length);
        path += L"\\dxgi.dll";
        g_systemDxgi = LoadLibraryW(path.c_str());

        HMODULE proxyModule{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LoadSystemDxgi), &proxyModule);
        if (g_systemDxgi == proxyModule)
            g_systemDxgi = nullptr;
    }

    bool HooksEnabled() noexcept
    {
        std::call_once(g_configOnce, []
        {
            logging::Initialize();
            wchar_t executablePath[32768]{};
            const DWORD length = GetModuleFileNameW(nullptr, executablePath,
                static_cast<DWORD>(std::size(executablePath)));
            if (length == 0 || length >= std::size(executablePath))
                return;

            std::wstring configPath(executablePath, length);
            const auto separator = configPath.find_last_of(L"\\/");
            if (separator == std::wstring::npos)
                return;
            configPath.resize(separator + 1);
            configPath += L"dayz_openxr.ini";
            hooks::ConfigureResolution(configPath);
            wchar_t value[16]{};
            GetPrivateProfileStringW(L"hooks", L"enabled", L"false", value,
                static_cast<DWORD>(std::size(value)), configPath.c_str());
            g_hooksEnabled = _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
                _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
            logging::Info(g_hooksEnabled ? "DXGI hooks enabled" : "DXGI hooks disabled");
        });
        return g_hooksEnabled;
    }
}

namespace proxy
{
    bool EnsureSystemDxgiLoaded() noexcept { std::call_once(g_once, LoadSystemDxgi); return g_systemDxgi != nullptr; }
    FARPROC Resolve(const char* name) noexcept { return EnsureSystemDxgiLoaded() ? GetProcAddress(g_systemDxgi, name) : nullptr; }
}

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    const auto original = reinterpret_cast<Fn>(proxy::Resolve("CreateDXGIFactory"));
    if (!original) return E_NOINTERFACE;
    const HRESULT result = original(riid, factory);
    if (SUCCEEDED(result) && factory && *factory && HooksEnabled())
        hooks::AttachToFactory(static_cast<IUnknown*>(*factory));
    return result;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory)
{
    logging::Initialize();
    logging::Info("Proxy CreateDXGIFactory1 called");
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    const auto original = reinterpret_cast<Fn>(proxy::Resolve("CreateDXGIFactory1"));
    if (!original) return E_NOINTERFACE;
    const HRESULT result = original(riid, factory);
    if (SUCCEEDED(result) && factory && *factory && HooksEnabled())
        hooks::AttachToFactory(static_cast<IUnknown*>(*factory));
    return result;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto original = reinterpret_cast<Fn>(proxy::Resolve("CreateDXGIFactory2"));
    if (!original) return E_NOINTERFACE;
    const HRESULT result = original(flags, riid, factory);
    if (SUCCEEDED(result) && factory && *factory && HooksEnabled())
        hooks::AttachToFactory(static_cast<IUnknown*>(*factory));
    return result;
}

HRESULT WINAPI DXGIDeclareAdapterRemovalSupport()
{
    using Fn = HRESULT(WINAPI*)();
    const auto original = reinterpret_cast<Fn>(proxy::Resolve("DXGIDeclareAdapterRemovalSupport"));
    return original ? original() : E_NOINTERFACE;
}

HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** value)
{
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto original = reinterpret_cast<Fn>(proxy::Resolve("DXGIGetDebugInterface1"));
    return original ? original(flags, riid, value) : E_NOINTERFACE;
}
