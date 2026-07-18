#include "dayz_frame_source.hpp"

#include "logging.hpp"
#include "stereo_state.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <sstream>

namespace
{
    constexpr char ShaderSource[] = R"(
Texture2D GameFrame : register(t0);
SamplerState LinearClamp : register(s0);

cbuffer FrameConstants : register(b0)
{
    float2 DisplayScale;
    float2 DisplayOffset;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target
{
    float2 sourceUv = (input.uv - DisplayOffset) / DisplayScale;
    if (any(sourceUv < 0.0) || any(sourceUv > 1.0))
        return float4(0.0, 0.0, 0.0, 1.0);
    return GameFrame.Sample(LinearClamp, sourceUv);
}
)";

    bool CompileShader(const char* entryPoint, const char* target,
        Microsoft::WRL::ComPtr<ID3DBlob>& bytecode) noexcept
    {
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(ShaderSource, sizeof(ShaderSource) - 1,
            "dayz_frame_source.hlsl", nullptr, nullptr, entryPoint, target,
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &bytecode, &errors);
        if (FAILED(result))
        {
            if (errors)
                logging::Error(std::string_view(static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize()));
            else
                logging::Error("D3DCompile failed for DayZFrameSource");
            return false;
        }
        return true;
    }
}

DayZFrameSource::DayZFrameSource(IDXGISwapChain* swapChain, ID3D11Device* device,
    ID3D11DeviceContext* immediateContext) noexcept
    : swapChain_(swapChain), device_(device), immediateContext_(immediateContext)
{
    pipelineReady_ = CreatePipeline();
}

bool DayZFrameSource::CreatePipeline() noexcept
{
    if (!swapChain_ || !device_ || !immediateContext_)
        return false;
    if (FAILED(device_->CreateDeferredContext(0, &deferredContext_)))
    {
        logging::Error("CreateDeferredContext failed; game-frame capture disabled");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vertexBytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelBytecode;
    if (!CompileShader("VSMain", "vs_5_0", vertexBytecode) ||
        !CompileShader("PSMain", "ps_5_0", pixelBytecode))
        return false;
    if (FAILED(device_->CreateVertexShader(vertexBytecode->GetBufferPointer(),
            vertexBytecode->GetBufferSize(), nullptr, &vertexShader_)) ||
        FAILED(device_->CreatePixelShader(pixelBytecode->GetBufferPointer(),
            pixelBytecode->GetBufferSize(), nullptr, &pixelShader_)))
        return false;

    D3D11_SAMPLER_DESC samplerDescription{};
    samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&samplerDescription, &sampler_)))
        return false;

    D3D11_BUFFER_DESC constantDescription{};
    constantDescription.ByteWidth = sizeof(FrameConstants);
    constantDescription.Usage = D3D11_USAGE_DEFAULT;
    constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device_->CreateBuffer(&constantDescription, nullptr, &constants_));
}

DXGI_FORMAT DayZFrameSource::ShaderResourceFormat(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return format;
    }
}

bool DayZFrameSource::EnsureCaptureTexture(
    const D3D11_TEXTURE2D_DESC& backBufferDescription) noexcept
{
    if (captureTextures_[0] && captureTextures_[1] &&
        sourceWidth_ == backBufferDescription.Width &&
        sourceHeight_ == backBufferDescription.Height &&
        sourceFormat_ == backBufferDescription.Format)
        return true;

    for (auto& view : captureViews_)
        view.Reset();
    for (auto& texture : captureTextures_)
        texture.Reset();
    ready_ = {};
    D3D11_TEXTURE2D_DESC description{};
    description.Width = backBufferDescription.Width;
    description.Height = backBufferDescription.Height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = backBufferDescription.Format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    viewDescription.Format = ShaderResourceFormat(description.Format);
    viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDescription.Texture2D.MostDetailedMip = 0;
    viewDescription.Texture2D.MipLevels = 1;
    for (std::size_t eye = 0; eye < captureTextures_.size(); ++eye)
    {
        if (FAILED(device_->CreateTexture2D(&description, nullptr, &captureTextures_[eye])) ||
            FAILED(device_->CreateShaderResourceView(captureTextures_[eye].Get(),
                &viewDescription, &captureViews_[eye])))
        {
            for (auto& view : captureViews_)
                view.Reset();
            for (auto& texture : captureTextures_)
                texture.Reset();
            return false;
        }
    }

    sourceWidth_ = description.Width;
    sourceHeight_ = description.Height;
    sourceFormat_ = description.Format;
    std::ostringstream message;
    message << "DayZ backbuffer capture ready: " << sourceWidth_ << 'x' << sourceHeight_
            << " format=" << static_cast<int>(sourceFormat_);
    logging::Info(message.str());
    return true;
}

void DayZFrameSource::PrepareFrame(std::uint32_t sourceEye) noexcept
{
    if (!pipelineReady_)
        return;
    sourceEye = (std::min)(sourceEye, 1u);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        return;
    D3D11_TEXTURE2D_DESC description{};
    backBuffer->GetDesc(&description);
    if (!EnsureCaptureTexture(description))
        return;

    if (description.SampleDesc.Count > 1)
    {
        immediateContext_->ResolveSubresource(captureTextures_[sourceEye].Get(), 0,
            backBuffer.Get(), 0,
            ShaderResourceFormat(description.Format));
    }
    else
    {
        immediateContext_->CopyResource(captureTextures_[sourceEye].Get(), backBuffer.Get());
    }
    ready_[sourceEye] = true;
}

void DayZFrameSource::RenderEye(const EyeRenderInfo& eye) noexcept
{
    if (!HasGameData() || !deferredContext_ || !eye.rtv || eye.width == 0 || eye.height == 0)
        return;

    FrameConstants values{{1.0f, 1.0f}, {0.0f, 0.0f}};
    const float sourceAspect = static_cast<float>(sourceWidth_) / sourceHeight_;
    const float targetAspect = static_cast<float>(eye.width) / eye.height;
    const dayz::stereo_state::Presentation presentation =
        dayz::stereo_state::GetPresentation();
    if (presentation.fitMode == dayz::stereo_state::FitMode::Contain)
    {
        if (sourceAspect > targetAspect)
            values.displayScale[1] = targetAspect / sourceAspect;
        else
            values.displayScale[0] = sourceAspect / targetAspect;
    }
    else if (presentation.fitMode == dayz::stereo_state::FitMode::Cover)
    {
        if (sourceAspect > targetAspect)
            values.displayScale[0] = sourceAspect / targetAspect;
        else
            values.displayScale[1] = targetAspect / sourceAspect;
    }
    values.displayScale[0] *= (std::max)(presentation.scaleX, 0.01f);
    values.displayScale[1] *= (std::max)(presentation.scaleY, 0.01f);
    values.displayOffset[0] = (1.0f - values.displayScale[0]) * 0.5f;
    values.displayOffset[1] = (1.0f - values.displayScale[1]) * 0.5f;
    const float imageShift = dayz::stereo_state::ImageShift();
    values.displayOffset[0] += eye.eyeIndex == 0 ? -imageShift : imageShift;

    deferredContext_->ClearState();
    deferredContext_->UpdateSubresource(constants_.Get(), 0, nullptr, &values, 0, 0);
    ID3D11RenderTargetView* renderTarget = eye.rtv;
    deferredContext_->OMSetRenderTargets(1, &renderTarget, nullptr);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(eye.width),
        static_cast<float>(eye.height), 0.0f, 1.0f};
    deferredContext_->RSSetViewports(1, &viewport);
    deferredContext_->IASetInputLayout(nullptr);
    deferredContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferredContext_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    deferredContext_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    const std::size_t requestedEye = (std::min)(static_cast<std::size_t>(eye.eyeIndex),
        captureViews_.size() - 1);
    const std::size_t availableEye = ready_[requestedEye] ? requestedEye : 1 - requestedEye;
    ID3D11ShaderResourceView* sourceView = captureViews_[availableEye].Get();
    ID3D11SamplerState* sampler = sampler_.Get();
    ID3D11Buffer* constants = constants_.Get();
    deferredContext_->PSSetShaderResources(0, 1, &sourceView);
    deferredContext_->PSSetSamplers(0, 1, &sampler);
    deferredContext_->PSSetConstantBuffers(0, 1, &constants);
    deferredContext_->Draw(3, 0);
    sourceView = nullptr;
    deferredContext_->PSSetShaderResources(0, 1, &sourceView);

    Microsoft::WRL::ComPtr<ID3D11CommandList> commands;
    if (SUCCEEDED(deferredContext_->FinishCommandList(FALSE, &commands)))
        immediateContext_->ExecuteCommandList(commands.Get(), TRUE);
}
