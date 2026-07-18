#include "debug_frame_source.hpp"

#include <algorithm>
#include <cmath>

DebugFrameSource::DebugFrameSource(ID3D11DeviceContext* context) noexcept : context_(context)
{
    if (context_)
        context_->QueryInterface(IID_PPV_ARGS(&context1_));
}

void DebugFrameSource::RenderEye(const EyeRenderInfo& eye) noexcept
{
    if (!context_ || !eye.rtv)
        return;

    const auto& q = eye.pose.orientation;
    const float red = std::clamp(0.5f + q.y, 0.05f, 0.95f);
    const float green = std::clamp(0.5f + q.x, 0.05f, 0.95f);
    const float blue = std::clamp(0.5f + q.z, 0.05f, 0.95f);
    const float color[4] = { red, green, blue, 1.0f };
    context_->ClearRenderTargetView(eye.rtv, color);

    if (!context1_)
        return;
    const float gridColor[4] = {0.12f, 0.12f, 0.12f, 1.0f};
    const float markerColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const LONG width = static_cast<LONG>(eye.width);
    const LONG height = static_cast<LONG>(eye.height);
    const LONG thin = std::max<LONG>(1, width / 600);
    for (LONG division = 1; division < 4; ++division)
    {
        const LONG x = width * division / 4;
        const LONG y = height * division / 4;
        const D3D11_RECT lines[] = {{x - thin, 0, x + thin, height}, {0, y - thin, width, y + thin}};
        context1_->ClearView(eye.rtv, gridColor, lines, static_cast<UINT>(std::size(lines)));
    }
    const LONG marker = std::max<LONG>(6, width / 80);
    const LONG centerX = width / 2;
    const LONG centerY = height / 2;
    const D3D11_RECT cross[] = {
        {centerX - marker, centerY - thin, centerX + marker, centerY + thin},
        {centerX - thin, centerY - marker, centerX + thin, centerY + marker}};
    context1_->ClearView(eye.rtv, markerColor, cross, static_cast<UINT>(std::size(cross)));
}
