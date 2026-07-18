#pragma once

#include <d3d11.h>
#include <openxr/openxr.h>

#include <cstdint>

struct EyeRenderInfo
{
    std::uint32_t eyeIndex{};
    XrPosef pose{};
    XrFovf fov{};
    ID3D11Texture2D* target{};
    ID3D11RenderTargetView* rtv{};
    std::uint32_t width{};
    std::uint32_t height{};
};

class IFrameSource
{
public:
    virtual ~IFrameSource() = default;
    virtual void PrepareFrame(std::uint32_t sourceEye = 0) noexcept { (void)sourceEye; }
    virtual bool HasGameData() const noexcept = 0;
    virtual void RenderEye(const EyeRenderInfo& eye) noexcept = 0;
};
