#pragma once

#include "frame_source.hpp"

#include <dxgi.h>
#include <array>
#include <wrl/client.h>

class DayZFrameSource final : public IFrameSource
{
public:
    DayZFrameSource(IDXGISwapChain* swapChain, ID3D11Device* device,
        ID3D11DeviceContext* immediateContext) noexcept;

    void PrepareFrame(std::uint32_t sourceEye = 0) noexcept override;
    bool HasGameData() const noexcept override { return ready_[0] || ready_[1]; }
    void RenderEye(const EyeRenderInfo& eye) noexcept override;

private:
    struct FrameConstants
    {
        float displayScale[2];
        float displayOffset[2];
    };

    bool CreatePipeline() noexcept;
    bool EnsureCaptureTexture(const D3D11_TEXTURE2D_DESC& backBufferDescription) noexcept;
    static DXGI_FORMAT ShaderResourceFormat(DXGI_FORMAT format) noexcept;

    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediateContext_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> deferredContext_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 2> captureTextures_;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2> captureViews_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    std::uint32_t sourceWidth_{};
    std::uint32_t sourceHeight_{};
    DXGI_FORMAT sourceFormat_{DXGI_FORMAT_UNKNOWN};
    std::array<bool, 2> ready_{};
    bool pipelineReady_{};
    bool loggedCapture_{};
};
