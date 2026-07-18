#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return message == WM_DESTROY ? (PostQuitMessage(0), 0)
                                 : DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    const HMODULE proxy = LoadLibraryW(L".\\proxy_test.dll");
    if (!proxy)
        return 1;
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const auto createFactory = reinterpret_cast<CreateFactoryFn>(
        GetProcAddress(proxy, "CreateDXGIFactory1"));
    if (!createFactory)
        return 2;
    const HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11)
        return 3;
    using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*,
        ID3D11DeviceContext**);
    const auto createDevice = reinterpret_cast<CreateDeviceFn>(
        GetProcAddress(d3d11, "D3D11CreateDevice"));
    if (!createDevice)
        return 4;

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"DayZOpenXRSmoke";
    if (!RegisterClassW(&windowClass))
        return 10;

    HWND window = CreateWindowW(windowClass.lpszClassName, L"DXGI smoke", WS_OVERLAPPEDWINDOW,
        0, 0, 640, 480, nullptr, nullptr, instance, nullptr);
    if (!window)
        return 11;

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = 640;
    description.BufferDesc.Height = 480;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.OutputWindow = window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    HRESULT result{};
    ComPtr<IDXGIFactory1> factory;
    result = createFactory(IID_PPV_ARGS(&factory));
    if (FAILED(result))
        return 19;
    ComPtr<IDXGIAdapter1> adapter;
    result = factory->EnumAdapters1(0, &adapter);
    if (FAILED(result))
        return 20;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel{};
    result = createDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(result))
        return 21;
    ComPtr<IDXGISwapChain> swapChain;
    result = factory->CreateSwapChain(device.Get(), &description, &swapChain);
    if (FAILED(result))
        return 22;

    for (int frame = 0; frame < 30; ++frame)
    {
        result = swapChain->Present(0, 0);
        if (FAILED(result))
            return 30;
    }
    result = swapChain->ResizeBuffers(2, 800, 600, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(result))
        return 40;
    for (int frame = 0; frame < 30; ++frame)
    {
        result = swapChain->Present(0, 0);
        if (FAILED(result))
            return 50;
    }

    HWND secondWindow = CreateWindowW(windowClass.lpszClassName, L"DXGI smoke 2",
        WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, instance, nullptr);
    if (!secondWindow)
        return 60;
    description.OutputWindow = secondWindow;
    description.BufferDesc.Width = 320;
    description.BufferDesc.Height = 240;
    ComPtr<IDXGISwapChain> secondSwapChain;
    result = factory->CreateSwapChain(device.Get(), &description, &secondSwapChain);
    if (FAILED(result))
        return 61;
    for (int frame = 0; frame < 30; ++frame)
    {
        result = secondSwapChain->Present(0, 0);
        if (FAILED(result))
            return 62;
    }
    secondSwapChain.Reset();
    DestroyWindow(secondWindow);
    DestroyWindow(window);
    return 0;
}
