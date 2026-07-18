#include "stereo_state.hpp"

#include <array>
#include <atomic>

namespace
{
    std::array<std::atomic<float>, 6> g_positions{};
    std::atomic_bool g_valid{};
    std::array<std::atomic<float>, 4> g_orientation{{0.0f, 0.0f, 0.0f, 1.0f}};
    std::atomic_uint g_orientationSequence{};
    std::atomic_uint g_eye{};
    std::atomic<float> g_imageShift{};
    std::atomic_uint g_fitMode{static_cast<unsigned>(dayz::stereo_state::FitMode::Contain)};
    std::atomic<float> g_scaleX{1.0f};
    std::atomic<float> g_scaleY{1.0f};
}

namespace dayz::stereo_state
{
    void UpdateEyePositions(float leftX, float leftY, float leftZ,
        float rightX, float rightY, float rightZ) noexcept
    {
        const float values[] = {leftX, leftY, leftZ, rightX, rightY, rightZ};
        for (std::size_t index = 0; index < std::size(values); ++index)
            g_positions[index].store(values[index], std::memory_order_relaxed);
        g_valid.store(true, std::memory_order_release);
    }

    EyePositions GetEyePositions() noexcept
    {
        EyePositions result{};
        result.valid = g_valid.load(std::memory_order_acquire);
        result.leftX = g_positions[0].load(std::memory_order_relaxed);
        result.rightX = g_positions[3].load(std::memory_order_relaxed);
        return result;
    }

    void UpdateHmdOrientation(float x, float y, float z, float w) noexcept
    {
        g_orientationSequence.fetch_add(1, std::memory_order_acq_rel);
        const float values[]{x, y, z, w};
        for (std::size_t index = 0; index < std::size(values); ++index)
            g_orientation[index].store(values[index], std::memory_order_relaxed);
        g_orientationSequence.fetch_add(1, std::memory_order_release);
    }

    HmdOrientation GetHmdOrientation() noexcept
    {
        HmdOrientation result{};
        for (;;)
        {
            const unsigned before = g_orientationSequence.load(std::memory_order_acquire);
            if (before & 1u)
                continue;
            result.x = g_orientation[0].load(std::memory_order_relaxed);
            result.y = g_orientation[1].load(std::memory_order_relaxed);
            result.z = g_orientation[2].load(std::memory_order_relaxed);
            result.w = g_orientation[3].load(std::memory_order_relaxed);
            const unsigned after = g_orientationSequence.load(std::memory_order_acquire);
            if (before == after)
            {
                result.valid = after != 0;
                return result;
            }
        }
    }

    unsigned RenderedEye() noexcept
    {
        return g_eye.load(std::memory_order_relaxed) & 1u;
    }

    void AdvanceEye() noexcept
    {
        g_eye.fetch_xor(1u, std::memory_order_relaxed);
    }

    void SetImageShift(float shift) noexcept
    {
        g_imageShift.store(shift, std::memory_order_relaxed);
    }

    float ImageShift() noexcept
    {
        return g_imageShift.load(std::memory_order_relaxed);
    }

    void SetPresentation(FitMode fitMode, float scaleX, float scaleY) noexcept
    {
        g_fitMode.store(static_cast<unsigned>(fitMode), std::memory_order_relaxed);
        g_scaleX.store(scaleX, std::memory_order_relaxed);
        g_scaleY.store(scaleY, std::memory_order_relaxed);
    }

    Presentation GetPresentation() noexcept
    {
        Presentation result{};
        result.fitMode = static_cast<FitMode>(g_fitMode.load(std::memory_order_relaxed));
        result.scaleX = g_scaleX.load(std::memory_order_relaxed);
        result.scaleY = g_scaleY.load(std::memory_order_relaxed);
        return result;
    }
}
