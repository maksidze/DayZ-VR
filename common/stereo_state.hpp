#pragma once

namespace dayz::stereo_state
{
    struct EyePositions
    {
        float leftX{};
        float rightX{};
        bool valid{};
    };

    struct HmdOrientation
    {
        float x{};
        float y{};
        float z{};
        float w{1.0f};
        bool valid{};
    };
    struct HmdPosition
    {
        float x{};
        float y{};
        float z{};
        bool valid{};
    };
    struct CameraDirections
    {
        float nativeX{};
        float nativeY{};
        float nativeZ{-1.0f};
        float renderX{};
        float renderY{};
        float renderZ{-1.0f};
        bool valid{};
    };

    enum class FitMode : unsigned { Contain, Stretch, Cover };
    struct Presentation
    {
        FitMode fitMode{FitMode::Contain};
        float scaleX{1.0f};
        float scaleY{1.0f};
    };

    void UpdateEyePositions(float leftX, float leftY, float leftZ,
        float rightX, float rightY, float rightZ) noexcept;
    EyePositions GetEyePositions() noexcept;
    void UpdateHmdOrientation(float x, float y, float z, float w) noexcept;
    HmdOrientation GetHmdOrientation() noexcept;
    void UpdateHmdPosition(float x, float y, float z) noexcept;
    HmdPosition GetHmdPosition() noexcept;
    void UpdateCameraDirections(float nativeX, float nativeY, float nativeZ,
        float renderX, float renderY, float renderZ) noexcept;
    CameraDirections GetCameraDirections() noexcept;
    unsigned RenderedEye() noexcept;
    void AdvanceEye() noexcept;
    void SetImageShift(float shift) noexcept;
    float ImageShift() noexcept;
    void SetPresentation(FitMode fitMode, float scaleX, float scaleY) noexcept;
    Presentation GetPresentation() noexcept;
}
