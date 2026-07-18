#pragma once

#include "frame_source.hpp"

#include <d3d11.h>
#include <dxgi1_6.h>
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <memory>
#include <mutex>
#include <vector>
#include <wrl/client.h>

class OpenXrHost
{
public:
    static OpenXrHost& Instance() noexcept;

    bool InitializeWithDevice(ID3D11Device* device) noexcept;
    void AttachGameSwapChain(IDXGISwapChain* swapChain) noexcept;
    bool InitializeStandalone() noexcept;
    void Tick() noexcept;
    void Shutdown() noexcept;
    bool IsInitialized() const noexcept { return initialized_; }
    bool IsSessionRunning() const noexcept { return sessionRunning_; }
    bool ShouldExit() const noexcept { return shouldExit_; }

private:
    struct EyeSwapchain
    {
        XrSwapchain handle{XR_NULL_HANDLE};
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<XrSwapchainImageD3D11KHR> images;
        std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> rtvs;
    };

    bool CreateInstanceAndSystem();
    bool CreateCompatibleDevice();
    bool ValidateDevice(ID3D11Device* device);
    bool FinishInitialization(ID3D11Device* device);
    bool CreateSession();
    bool CreateSpaces();
    bool CreateSwapchains();
    void PollEvents();
    void RenderFrame();
    bool Check(XrResult result, const char* operation) const noexcept;

    mutable std::mutex mutex_;
    XrInstance instance_{XR_NULL_HANDLE};
    XrSystemId systemId_{XR_NULL_SYSTEM_ID};
    XrSession session_{XR_NULL_HANDLE};
    XrSpace localSpace_{XR_NULL_HANDLE};
    XrSpace viewSpace_{XR_NULL_HANDLE};
    XrSessionState sessionState_{XR_SESSION_STATE_UNKNOWN};
    PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11Requirements_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> gameSwapChain_;
    std::unique_ptr<IFrameSource> debugFrameSource_;
    std::unique_ptr<IFrameSource> gameFrameSource_;
    std::array<EyeSwapchain, 2> eyeSwapchains_{};
    std::array<XrView, 2> views_{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    bool initialized_{};
    bool sessionRunning_{};
    bool shouldExit_{};
};
