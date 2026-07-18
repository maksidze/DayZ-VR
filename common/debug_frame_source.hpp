#pragma once

#include "frame_source.hpp"

#include <d3d11_1.h>
#include <wrl/client.h>

class DebugFrameSource final : public IFrameSource
{
public:
    explicit DebugFrameSource(ID3D11DeviceContext* context) noexcept;
    bool HasGameData() const noexcept override { return false; }
    void RenderEye(const EyeRenderInfo& eye) noexcept override;

private:
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;
};
