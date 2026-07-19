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

    struct GuiSwapchain
    {
        XrSwapchain handle{XR_NULL_HANDLE};
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<XrSwapchainImageD3D11KHR> images;
        std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> rtvs;
    };

    struct AxisSwapchain
    {
        XrSwapchain handle{XR_NULL_HANDLE};
        std::vector<XrSwapchainImageD3D11KHR> images;
    };

    bool CreateInstanceAndSystem();
    bool CreateCompatibleDevice();
    bool ValidateDevice(ID3D11Device* device);
    bool FinishInitialization(ID3D11Device* device);
    bool CreateSession();
    bool CreateSpaces();
    bool CreateSwapchains();
    bool CreateGuiSwapchain(const std::vector<std::int64_t>& formats);
    bool CreateControllerActions();
    bool CreateAxisSwapchain(const std::vector<std::int64_t>& formats);
    void SyncControllerInput(XrTime displayTime, bool guiVisible);
    void ReleaseControllerKeys() noexcept;
    void PollEvents();
    void RenderFrame();
    void AnchorGuiQuad(const XrPosef& headPose) noexcept;
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
    GuiSwapchain guiSwapchain_{};
    AxisSwapchain axisSwapchain_{};
    XrActionSet actionSet_{XR_NULL_HANDLE};
    XrAction gripPoseAction_{XR_NULL_HANDLE};
    XrAction aimPoseAction_{XR_NULL_HANDLE};
    XrAction triggerAction_{XR_NULL_HANDLE};
    XrAction xButtonAction_{XR_NULL_HANDLE};
    XrAction yButtonAction_{XR_NULL_HANDLE};
    XrAction aButtonAction_{XR_NULL_HANDLE};
    XrAction bButtonAction_{XR_NULL_HANDLE};
    XrAction thumbstickAction_{XR_NULL_HANDLE};
    std::array<XrPath, 2> handPaths_{{XR_NULL_PATH, XR_NULL_PATH}};
    std::array<XrSpace, 2> gripSpaces_{{XR_NULL_HANDLE, XR_NULL_HANDLE}};
    std::array<XrSpace, 2> aimSpaces_{{XR_NULL_HANDLE, XR_NULL_HANDLE}};
    std::array<XrSpaceLocation, 2> gripLocations_{{
        {XR_TYPE_SPACE_LOCATION}, {XR_TYPE_SPACE_LOCATION}}};
    std::array<XrSpaceLocation, 2> aimLocations_{{
        {XR_TYPE_SPACE_LOCATION}, {XR_TYPE_SPACE_LOCATION}}};
    std::array<XrView, 2> views_{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    XrPosef guiQuadPose_{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.25f}};
    float guiQuadWidthMeters_{1.4f};
    float guiQuadDistance_{1.25f};
    float guiQuadVerticalOffset_{-0.1f};
    bool guiQuadEnabled_{true};
    bool guiQuadAnchored_{};
    bool guiQuadWasVisible_{};
    bool guiQuadHasImage_{};
    bool controllerInputEnabled_{true};
    bool controllerAxesEnabled_{true};
    bool guiRayEnabled_{true};
    float guiRayLength_{2.0f};
    float guiRayThickness_{0.004f};
    bool guiRayValid_{};
    float currentGuiRayLength_{2.0f};
    bool directionRaysEnabled_{true};
    float directionRayLength_{3.0f};
    float directionRayThickness_{0.006f};
    float controllerTurnScale_{18.0f};
    float controllerDeadzone_{0.3f};
    std::array<bool, 4> movementKeys_{};
    bool triggerDown_{};
    bool xButtonDown_{};
    bool yButtonDown_{};
    bool aButtonDown_{};
    bool bButtonDown_{};
    bool initialized_{};
    bool sessionRunning_{};
    bool shouldExit_{};
};
