#include "dayz_runtime_probe.hpp"

#include "logging.hpp"
#include "stereo_state.hpp"

#include <MinHook.h>
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <intrin.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <cmath>
#include <sstream>
#include <string>
#include <mutex>
#include <wrl/client.h>

namespace
{
    struct BuildProfile
    {
        const char* name;
        std::uint32_t peTimestamp;
        std::uint32_t imageSize;
        std::uintptr_t prepareViewRva;
        std::uintptr_t executeViewRva;
        std::uintptr_t finalizeViewRva;
        std::uintptr_t projectionDispatchRva;
        std::uintptr_t hudLayoutRva;
        std::uintptr_t guiInputMessageRva;
        std::uintptr_t guiScaleRva;
        std::uintptr_t engineSingletonRva;
        std::uintptr_t inventoryPreviewPrepareCallerRva;
        std::uintptr_t dynamicBlurRva;
        std::uintptr_t dynamicBlurParameterIndexRva;
        std::uintptr_t profileFovRva;
        std::uintptr_t cameraManagerRva;
        std::uintptr_t getActiveCameraStateRva;
        std::uintptr_t cameraFovUpdateRva;
    };

    constexpr std::array kBuildProfiles{
        BuildProfile{"DayZ_x64", 0x6A47B9AAu, 0x04407000u, 0x004501A0, 0x004513A0,
            0x004514B0, 0x00952B30, 0x008A2DB0, 0x00351760, 0x0427063C,
            0x04263740, 0x005C5899, 0x0022FB70, 0x00FEE968, 0x01008E70,
            0x01008D50, 0x004B77D0, 0x004B86C0},
        BuildProfile{"DayZDiag_x64", 0x6A47BAF9u, 0x049E7000u, 0x0048D8A0, 0x0048EB30,
            0x0048EC40, 0x00B30D10, 0x00A7B7B0, 0x00389410, 0x04823F4C,
            0x04815740, 0, 0, 0, 0, 0, 0, 0},
    };

    const BuildProfile* g_buildProfile{};
    std::uint32_t kImageSize{};
    std::uintptr_t kPrepareViewRva{};
    std::uintptr_t kExecuteViewRva{};
    std::uintptr_t kFinalizeViewRva{};
    std::uintptr_t kProjectionDispatchRva{};
    std::uintptr_t kHudLayoutRva{};
    std::uintptr_t kGuiInputMessageRva{};
    std::uintptr_t kGuiScaleRva{};
    std::uintptr_t kEngineSingletonRva{};
    std::uintptr_t kInventoryPreviewPrepareCallerRva{};
    std::uintptr_t kDynamicBlurRva{};
    std::uintptr_t kDynamicBlurParameterIndexRva{};
    std::uintptr_t kProfileFovRva{};
    std::uintptr_t kCameraManagerRva{};
    std::uintptr_t kGetActiveCameraStateRva{};
    std::uintptr_t kCameraFovUpdateRva{};
    constexpr std::ptrdiff_t kContextCamera = 0x118;
    constexpr std::ptrdiff_t kPreparedContextCamera = 0xA34;
    constexpr std::ptrdiff_t kContextDescriptor = 0xA10;
    constexpr std::ptrdiff_t kContextArenaCursor = 0xA88;

    struct OpaqueEngine;
    struct OpaqueContext;
    struct OpaqueCamera;

    using PrepareViewFn = void(__fastcall*)(OpaqueEngine*, OpaqueContext*, std::uint8_t,
        OpaqueCamera*);
    using ExecuteViewFn = void*(__fastcall*)(OpaqueEngine*, OpaqueContext*, std::uint8_t);
    using FinalizeViewFn = std::uintptr_t(__fastcall*)(OpaqueEngine*, OpaqueContext*,
        std::uint8_t);
    using ProjectionDispatchFn = std::uintptr_t(__fastcall*)(OpaqueContext*, std::uint8_t);
    using CameraFovUpdateFn = bool(__fastcall*)(void*);
    using FrameRefreshFn = std::uintptr_t(__fastcall*)(OpaqueCamera*, void*, std::uint8_t);
    using HudLayoutFn = char(__fastcall*)(void*);
    using CameraRefreshFn = float*(__fastcall*)(OpaqueCamera*, void*);
    using GuiInputMessageFn = std::int64_t(__fastcall*)(void*, std::int64_t, unsigned int,
        std::int64_t, std::uintptr_t);
    using DynamicBlurFn = void(__fastcall*)(std::int64_t, unsigned int, std::int64_t,
        std::int64_t, int, int, int, std::int64_t);

    enum class EventKind : std::uint8_t { Projection, Prepare, Execute, Finalize };
    struct Event
    {
        EventKind kind{};
        std::uint8_t mode{};
        DWORD thread{};
        void* context{};
        void* camera{};
        void* descriptor{};
        std::uintptr_t arenaCursor{};
        std::uintptr_t callerRva{};
    };

    constexpr std::size_t kEventCapacity = 96;
    std::array<Event, kEventCapacity> g_events{};
    std::atomic_uint32_t g_eventCount{};
    std::atomic_uint64_t g_presentCount{};
    std::atomic_uint g_guiCursorVisibleFrames{};
    std::atomic_bool g_guiCursorDebounced{};
    std::atomic_bool g_active{};
    std::atomic_bool g_attempted{};
    std::uintptr_t g_moduleBase{};
    PrepareViewFn g_prepareView{};
    ExecuteViewFn g_executeView{};
    FinalizeViewFn g_finalizeView{};
    ProjectionDispatchFn g_projectionDispatch{};
    CameraFovUpdateFn g_cameraFovUpdate{};
    FrameRefreshFn g_frameRefresh{};
    HudLayoutFn g_hudLayout{};
    CameraRefreshFn g_cameraRefresh{};
    GuiInputMessageFn g_guiInputMessage{};
    DynamicBlurFn g_dynamicBlur{};
    std::atomic_bool g_cameraRefreshHookAttempted{};
    std::atomic_bool g_frameRefreshHookActive{};
    std::uintptr_t g_frameRefreshTarget{};
    Microsoft::WRL::ComPtr<ID3D11Device> g_d3dDevice;

    using OmSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
        ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
    using RsSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
        const D3D11_VIEWPORT*);
    using ClearRtvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*,
        const FLOAT[4]);
    using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*,
        UINT, FLOAT, UINT8);
    using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*,
        ID3D11Resource*);
    using ResolveFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
        ID3D11Resource*, UINT, DXGI_FORMAT);
    using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
    using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
    OmSetRenderTargetsFn g_omSetRenderTargets{};
    RsSetViewportsFn g_rsSetViewports{};
    ClearRtvFn g_clearRtv{};
    ClearDsvFn g_clearDsv{};
    CopyResourceFn g_copyResource{};
    ResolveFn g_resolve{};
    DrawIndexedFn g_drawIndexed{};
    DrawFn g_draw{};

    enum class ApiKind : std::uint8_t
    {
        Targets, Viewport, ClearColor, ClearDepth, Copy, Resolve, DrawIndexed, Draw
    };
    struct ApiEvent
    {
        ApiKind kind{};
        DWORD thread{};
        void* object0{};
        void* object1{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t format{};
        std::uint32_t samples{};
        std::uint32_t targetWidth{};
        std::uint32_t targetHeight{};
        std::uint32_t elementCount{};
        std::uintptr_t callerRva{};
    };
    constexpr std::size_t kApiEventCapacity = 256;
    std::array<ApiEvent, kApiEventCapacity> g_apiEvents{};
    std::atomic_uint32_t g_apiEventCount{};
    struct DrawStateEvent
    {
        ApiKind kind{};
        DWORD thread{};
        void* pixelShader{};
        void* vertexShader{};
        void* blendState{};
        void* depthState{};
        std::uint32_t targetWidth{};
        std::uint32_t targetHeight{};
        std::uint32_t targetFormat{};
        std::uint32_t elementCount{};
        std::uintptr_t callerRva{};
        std::uintptr_t parentCallerRva{};
        float viewportX{};
        float viewportY{};
        float viewportWidth{};
        float viewportHeight{};
        bool depthEnabled{};
        bool depthWrite{};
    };
    constexpr std::size_t kDrawStateCapacity = 64;
    std::array<DrawStateEvent, kDrawStateCapacity> g_drawStateEvents{};
    std::atomic_uint32_t g_drawStateEventCount{};
    std::atomic_bool g_captureApi{};
    bool g_alternateEyeEnabled{};
    bool g_hmdRotationEnabled{};
    bool g_hmdNativeAimEnabled{true};
    float g_hmdMouseYawScale{-600.0f};
    float g_hmdMousePitchScale{-600.0f};
    bool g_haveNativeHmdAngles{};
    float g_previousHmdYaw{};
    float g_previousHmdPitch{};
    double g_pendingMouseX{};
    double g_pendingMouseY{};
    float g_cameraSeparation{0.064f};
    float g_hmdPositionScale{1.0f};
    float g_gameFov{};
    float g_hudScale{0.85f};
    bool g_overrideHudScale{};
    float g_hudSafeWidth{};
    float g_hudSafeHeight{};
    float g_hudCompositeWidth{};
    float g_hudCompositeHeight{};
    float g_hudContentScale{1.0f};
    float g_hudLeftOffsetX{};
    float g_hudRightOffsetX{};
    std::atomic_bool g_hudSafeLogged{};

    std::mutex g_hudLayerMutex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> g_hudLayerTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> g_hudLayerTarget;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> g_hudLayerResolved;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> g_hudLayerView;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_hudCompositeContext;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> g_hudCompositeVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> g_hudCompositePs;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> g_hudCompositeSampler;
    Microsoft::WRL::ComPtr<ID3D11BlendState> g_hudCompositeBlend;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> g_hudCompositeDepth;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> g_hudCompositeRasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> g_hudCompositeConstants;
    std::uint32_t g_hudLayerWidth{};
    std::uint32_t g_hudLayerHeight{};
    DXGI_SAMPLE_DESC g_hudLayerSamples{};
    std::atomic_bool g_hudLayerDirty{};
    std::atomic_bool g_hudLayerNeedsClear{true};
    std::atomic_bool g_hudLayerLogged{};
    std::atomic_bool g_inventoryLayerLogged{};
    bool g_guiQuadEnabled{true};
    bool g_inventoryHmdLookEnabled{true};
    bool g_inventoryBlurEnabled{};
    bool g_inventoryPlayerPreviewVisible{true};
    float g_inventoryPreviewRotationScale{0.5f};
    std::atomic_bool g_inventoryPreviewActive{};
    unsigned int g_inventoryPreviewOrdinal{};
    std::mutex g_guiLayerMutex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> g_guiLayerTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> g_guiLayerTarget;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> g_guiLayerResolved;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> g_guiLayerView;
    std::uint32_t g_guiLayerWidth{};
    std::uint32_t g_guiLayerHeight{};
    DXGI_SAMPLE_DESC g_guiLayerSamples{};
    std::atomic_bool g_guiLayerDirty{};
    std::atomic_bool g_guiLayerNeedsClear{true};
    std::atomic_bool g_guiLayerLogged{};
    std::atomic_uint64_t g_guiLayerCapturedPresent{~std::uint64_t{}};
    bool g_guiMouseRemapEnabled{true};
    bool g_guiCursorEnabled{true};
    HWND g_gameWindow{};
    WNDPROC g_originalWindowProcedure{};
    using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
    using GetCursorInfoFn = BOOL(WINAPI*)(PCURSORINFO);
    GetCursorPosFn g_getCursorPos{};
    GetCursorInfoFn g_getCursorInfo{};
    std::atomic_uint32_t g_guiNativeWidth{};
    std::atomic_uint32_t g_guiNativeHeight{};
    std::atomic_uint32_t g_guiBackWidth{};
    std::atomic_uint32_t g_guiBackHeight{};
    std::atomic_long g_guiVirtualCursorX{};
    std::atomic_long g_guiVirtualCursorY{};
    std::atomic_bool g_guiVirtualCursorActive{};
    std::atomic_bool g_guiVirtualCursorApiLogged{};

    bool RawGuiCursorModeActive() noexcept;
    bool IsGuiCursorModeActive() noexcept;

    struct HudCompositeConstants
    {
        float cursorUv[2];
        float cursorPixelUv[2];
        float contentScale;
        float cursorVisible;
        float padding[2];
    };

    constexpr char kHudCompositeShader[] = R"(
Texture2D HudLayer : register(t0);
SamplerState LinearClamp : register(s0);
cbuffer HudConstants : register(b0)
{
    float2 CursorUv;
    float2 CursorPixelUv;
    float ContentScale;
    float CursorVisible;
    float2 Padding;
};
struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VertexOutput VSMain(uint id : SV_VertexID)
{
    VertexOutput output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
float4 PSMain(VertexOutput input) : SV_Target
{
    float2 sourceUv = (input.uv - 0.5) / ContentScale + 0.5;
    if (any(sourceUv < 0.0) || any(sourceUv > 1.0)) discard;
    float4 color = HudLayer.Sample(LinearClamp, sourceUv);
    float2 cursorPixel = (sourceUv - CursorUv) / CursorPixelUv;
    bool cursorBody = CursorVisible > 0.5 && cursorPixel.x >= 0.0 && cursorPixel.y >= 0.0 &&
        cursorPixel.y < 25.0 && cursorPixel.x <= cursorPixel.y * 0.68;
    bool cursorStem = CursorVisible > 0.5 && cursorPixel.x >= 6.0 && cursorPixel.x < 10.0 &&
        cursorPixel.y >= 14.0 && cursorPixel.y < 27.0;
    if (cursorBody || cursorStem)
    {
        bool whiteBody = cursorPixel.x >= 2.0 && cursorPixel.y >= 2.0 &&
            cursorPixel.y < 21.0 && cursorPixel.x <= cursorPixel.y * 0.60;
        bool whiteStem = cursorPixel.x >= 7.0 && cursorPixel.x < 9.0 &&
            cursorPixel.y >= 14.0 && cursorPixel.y < 24.0;
        return (whiteBody || whiteStem) ? float4(1.0, 1.0, 1.0, 1.0) :
            float4(0.0, 0.0, 0.0, 1.0);
    }
    if (color.a <= 0.0001) discard;
    float3 straightLinear = saturate(color.rgb / max(color.a, 0.0001));
    float3 encodedPremultiplied = pow(straightLinear, 1.0 / 2.2) * color.a;
    return float4(encodedPremultiplied, color.a);
}
)";

    struct Vec3 { float x; float y; float z; };
    struct Quaternion { float x; float y; float z; float w; };
    struct CameraBasis { Vec3 right; Vec3 up; Vec3 forward; };
    void* g_lastStereoCamera{};
    Vec3 g_baseTranslation{};
    Vec3 g_lastStereoTranslation{};
    bool g_haveStereoTranslation{};
    dayz::stereo_state::HmdPosition g_hmdPositionCenter{};
    Quaternion g_hmdPositionOrientationCenter{0.0f, 0.0f, 0.0f, 1.0f};
    bool g_haveHmdPositionCenter{};
    std::atomic_uint64_t g_stereoApplyCount{};
    Quaternion g_hmdCenter{0.0f, 0.0f, 0.0f, 1.0f};
    bool g_haveHmdCenter{};
    thread_local unsigned g_projectionRefreshDepth{};
    thread_local OpaqueCamera* g_projectionContextCamera{};
    std::array<std::atomic<std::uintptr_t>, 16> g_cameraRefreshCallers{};
    void* g_lastHmdCamera{};
    CameraBasis g_baseCameraBasis{};
    CameraBasis g_lastAppliedCameraBasis{};
    bool g_haveAppliedCameraBasis{};
    void* g_directionCalibrationCamera{};
    CameraBasis g_directionCalibrationBasis{};
    Quaternion g_directionCalibrationXr{0.0f, 0.0f, 0.0f, 1.0f};
    bool g_haveDirectionCalibration{};
    std::atomic_uint64_t g_hmdApplyCount{};
    Quaternion g_inventoryPreviewHmdCenter{0.0f, 0.0f, 0.0f, 1.0f};
    Quaternion g_guiLookHmdCenter{0.0f, 0.0f, 0.0f, 1.0f};
    bool g_haveGuiLookCenter{};
    OpaqueCamera* g_inventoryPreviewCamera{};
    CameraBasis g_inventoryPreviewBaseBasis{};
    CameraBasis g_inventoryPreviewLastBasis{};
    bool g_haveInventoryPreviewCenter{};
    bool g_haveInventoryPreviewBasis{};
    std::atomic_bool g_profileFovLogged{};
    std::atomic_bool g_activeCameraFovLogged{};

    void LogProfileFovOverride(float original) noexcept
    {
        std::ostringstream message;
        message << "DayZ profile FOV overridden in memory: " << original << " -> "
            << g_gameFov << " radians; profile file remains unchanged";
        logging::Info(message.str());
    }

    void ApplyProfileFovOverride() noexcept
    {
        if (!kProfileFovRva || !std::isfinite(g_gameFov) || g_gameFov <= 0.0f)
            return;
        float original{};
        bool applied{};
        __try
        {
            float* value = reinterpret_cast<float*>(g_moduleBase + kProfileFovRva);
            original = *value;
            *value = g_gameFov;
            applied = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            applied = false;
        }
        if (applied && !g_profileFovLogged.exchange(true))
            LogProfileFovOverride(original);
    }

    void LogActiveCameraFovOverride(float original, float zoomMultiplier) noexcept
    {
        std::ostringstream message;
        message << "DayZ active camera base FOV overridden: " << original << " -> "
            << g_gameFov << " radians; zoom_multiplier=" << zoomMultiplier;
        logging::Info(message.str());
    }

    void ApplyActiveCameraFovOverride() noexcept
    {
        if (!kCameraManagerRva || !kGetActiveCameraStateRva ||
            !std::isfinite(g_gameFov) || g_gameFov <= 0.0f)
            return;
        float original{};
        float zoomMultiplier{};
        bool applied{};
        __try
        {
            using GetActiveCameraStateFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
            const auto getActive = reinterpret_cast<GetActiveCameraStateFn>(
                g_moduleBase + kGetActiveCameraStateRva);
            const std::uintptr_t state = getActive(g_moduleBase + kCameraManagerRva);
            if (!state)
                return;
            original = *reinterpret_cast<float*>(state + 76);
            zoomMultiplier = *reinterpret_cast<float*>(state + 80);
            *reinterpret_cast<float*>(state + 76) = g_gameFov;
            applied = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            applied = false;
        }
        if (applied && !g_activeCameraFovLogged.exchange(true))
            LogActiveCameraFovOverride(original, zoomMultiplier);
    }

    bool __fastcall HookedCameraFovUpdate(void* cameraManager)
    {
        ApplyProfileFovOverride();
        ApplyActiveCameraFovOverride();
        return g_cameraFovUpdate(cameraManager);
    }

    void LogStereoApplication(unsigned eye, float offset)
    {
        std::ostringstream message;
        message << "Alternating eye camera applied eye=" << eye << " offset=" << offset;
        logging::Info(message.str());
    }

    void WriteHudScale() noexcept
    {
        __try
        {
            *reinterpret_cast<float*>(g_moduleBase + kGuiScaleRva) = g_hudScale;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    bool ApplyHudSafeArea(void* renderer, int* appliedWidth = nullptr,
        int* appliedHeight = nullptr) noexcept
    {
        const bool overrideSafeArea = g_hudSafeWidth > 0.0f && g_hudSafeHeight > 0.0f;
        if (!renderer || (!g_overrideHudScale && !overrideSafeArea))
            return false;
        __try
        {
            void** table = *reinterpret_cast<void***>(renderer);
            if (!table)
                return false;
            using DimensionFn = int(__fastcall*)(void*);
            const int width = reinterpret_cast<DimensionFn>(table[968 / sizeof(void*)])(renderer);
            const int height = reinterpret_cast<DimensionFn>(table[976 / sizeof(void*)])(renderer);
            if (width <= 0 || height <= 0)
                return false;

            float contentWidth = 1.0f;
            float contentHeight = 1.0f;
            if (g_overrideHudScale)
            {
                const float scale = (std::clamp)(g_hudScale, 0.05f, 1.0f);
                // Match DayZ's native IGUIScale layout calculation. At wide aspect ratios
                // the vertical extent is the requested scale and the horizontal extent is
                // reduced so GUI coordinates retain their intended 4:3 aspect.
                const float aspectCorrection =
                    (static_cast<float>(width) / static_cast<float>(height)) * 0.75f;
                if (aspectCorrection < 1.0f)
                {
                    contentWidth = scale;
                    contentHeight = scale * aspectCorrection;
                }
                else
                {
                    contentWidth = scale / aspectCorrection;
                    contentHeight = scale;
                }
            }
            if (overrideSafeArea)
            {
                const float safeWidth =
                    (std::clamp)(g_hudSafeWidth / static_cast<float>(width), 0.05f, 1.0f);
                const float safeHeight =
                    (std::clamp)(g_hudSafeHeight / static_cast<float>(height), 0.05f, 1.0f);
                contentWidth = (std::min)(contentWidth, safeWidth);
                contentHeight = (std::min)(contentHeight, safeHeight);
            }

            const float left = (1.0f - contentWidth) * 0.5f;
            const float top = (1.0f - contentHeight) * 0.5f;
            auto* values = reinterpret_cast<float*>(renderer);
            values[30] = left;
            values[31] = top;
            values[32] = 1.0f - left;
            values[33] = 1.0f - top;
            if (appliedWidth)
                *appliedWidth = width;
            if (appliedHeight)
                *appliedHeight = height;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void* ReadEngineSingleton() noexcept
    {
        __try
        {
            return *reinterpret_cast<void**>(g_moduleBase + kEngineSingletonRva);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void ApplyHudSafeAreaFromEngineSingleton() noexcept
    {
        void* renderer = ReadEngineSingleton();
        int width{};
        int height{};
        if (!ApplyHudSafeArea(renderer, &width, &height) || g_hudSafeLogged.exchange(true))
            return;
        std::ostringstream message;
        const auto* values = reinterpret_cast<const float*>(renderer);
        message << "HUD layout forced every frame: scale="
            << (g_overrideHudScale ? g_hudScale : 1.0f)
            << " safe_limit=" << g_hudSafeWidth << 'x' << g_hudSafeHeight
            << " framebuffer=" << width << 'x' << height
            << " rect=" << values[30] << ',' << values[31] << '-'
            << values[32] << ',' << values[33] << " renderer=" << renderer;
        logging::Info(message.str());
    }

    bool RuntimeProbeEnabled() noexcept
    {
        wchar_t executablePath[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executablePath,
            static_cast<DWORD>(std::size(executablePath)));
        if (length == 0 || length >= std::size(executablePath))
            return false;
        wchar_t* separator = wcsrchr(executablePath, L'\\');
        if (!separator)
            separator = wcsrchr(executablePath, L'/');
        if (!separator)
            return false;
        wcscpy_s(separator + 1, std::size(executablePath) - (separator + 1 - executablePath),
            L"dayz_openxr.ini");
        wchar_t value[16]{};
        GetPrivateProfileStringW(L"stereo", L"runtime_probe", L"true", value,
            static_cast<DWORD>(std::size(value)), executablePath);
        return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
            _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
    }

    std::wstring ConfigurationFile() noexcept
    {
        wchar_t executablePath[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executablePath,
            static_cast<DWORD>(std::size(executablePath)));
        if (length == 0 || length >= std::size(executablePath))
            return L"dayz_openxr.ini";
        wchar_t* separator = wcsrchr(executablePath, L'\\');
        if (!separator)
            separator = wcsrchr(executablePath, L'/');
        if (!separator)
            return L"dayz_openxr.ini";
        *(separator + 1) = L'\0';
        return std::wstring(executablePath) + L"dayz_openxr.ini";
    }

    bool ReadBoolean(const wchar_t* section, const wchar_t* key, bool fallback) noexcept
    {
        wchar_t value[16]{};
        const std::wstring path = ConfigurationFile();
        GetPrivateProfileStringW(section, key, fallback ? L"true" : L"false", value,
            static_cast<DWORD>(std::size(value)), path.c_str());
        return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
            _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
    }

    float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback) noexcept
    {
        wchar_t fallbackText[32]{};
        swprintf_s(fallbackText, L"%.4f", fallback);
        wchar_t value[32]{};
        const std::wstring path = ConfigurationFile();
        GetPrivateProfileStringW(section, key, fallbackText, value,
            static_cast<DWORD>(std::size(value)), path.c_str());
        wchar_t* end{};
        const float parsed = std::wcstof(value, &end);
        return end != value && std::isfinite(parsed) ? parsed : fallback;
    }

    std::wstring ReadString(const wchar_t* section, const wchar_t* key,
        const wchar_t* fallback) noexcept
    {
        wchar_t value[32]{};
        const std::wstring path = ConfigurationFile();
        GetPrivateProfileStringW(section, key, fallback, value,
            static_cast<DWORD>(std::size(value)), path.c_str());
        return value;
    }

    bool Match(std::uintptr_t rva, const std::uint8_t* bytes, const char* mask,
        std::size_t length) noexcept
    {
        const auto* address = reinterpret_cast<const std::uint8_t*>(g_moduleBase + rva);
        for (std::size_t index = 0; index < length; ++index)
            if (mask[index] == 'x' && address[index] != bytes[index])
                return false;
        return true;
    }

    std::uintptr_t ResolveFrameRefreshTarget() noexcept
    {
        // ProjectionDispatch is a small wrapper whose only direct relative call
        // refreshes the FrameBase and builds all cached view/projection matrices.
        const auto* code = reinterpret_cast<const std::uint8_t*>(
            g_moduleBase + kProjectionDispatchRva);
        for (std::size_t offset = 0; offset + 5 <= 64; ++offset)
        {
            if (code[offset] != 0xE8)
                continue;
            const std::int32_t displacement = *reinterpret_cast<const std::int32_t*>(
                code + offset + 1);
            const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(code + offset + 5) +
                displacement;
            if (target >= g_moduleBase && target < g_moduleBase + kImageSize)
                return target;
        }
        return 0;
    }

    bool ValidateBuild() noexcept
    {
        g_moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!g_moduleBase)
            return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_moduleBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_moduleBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        for (const BuildProfile& profile : kBuildProfiles)
            if (nt->FileHeader.TimeDateStamp == profile.peTimestamp &&
                nt->OptionalHeader.SizeOfImage == profile.imageSize)
            {
                g_buildProfile = &profile;
                break;
            }
        if (!g_buildProfile)
            return false;

        kPrepareViewRva = g_buildProfile->prepareViewRva;
        kImageSize = g_buildProfile->imageSize;
        kExecuteViewRva = g_buildProfile->executeViewRva;
        kFinalizeViewRva = g_buildProfile->finalizeViewRva;
        kProjectionDispatchRva = g_buildProfile->projectionDispatchRva;
        kHudLayoutRva = g_buildProfile->hudLayoutRva;
        kGuiInputMessageRva = g_buildProfile->guiInputMessageRva;
        kGuiScaleRva = g_buildProfile->guiScaleRva;
        kEngineSingletonRva = g_buildProfile->engineSingletonRva;
        kInventoryPreviewPrepareCallerRva =
            g_buildProfile->inventoryPreviewPrepareCallerRva;
        kDynamicBlurRva = g_buildProfile->dynamicBlurRva;
        kDynamicBlurParameterIndexRva = g_buildProfile->dynamicBlurParameterIndexRva;
        kProfileFovRva = g_buildProfile->profileFovRva;
        kCameraManagerRva = g_buildProfile->cameraManagerRva;
        kGetActiveCameraStateRva = g_buildProfile->getActiveCameraStateRva;
        kCameraFovUpdateRva = g_buildProfile->cameraFovUpdateRva;

        static constexpr std::uint8_t prepare[] = {
            0x48,0x85,0xD2,0x0F,0,0,0,0,0,0x48,0x8B,0xC4,0x55,0x56,0x57,0x41,
            0x56,0x41,0x57,0x48,0x8D,0xA8,0x58,0xFC};
        static constexpr std::uint8_t execute[] = {
            0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,
            0x18,0x48,0x89,0x7C,0x24,0x20,0x41,0x56,0x48,0x83,0xEC,0,0,0,0,0x48,
            0x8B,0xF9,0x48,0x8B,0xCA,0x41,0x0F,0xB6,0xE8,0x4C};
        static constexpr std::uint8_t finalize[] = {
            0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x41,0x0F,0xB6,0xF8,
            0x48,0x8B,0xDA,0x45,0x84,0xC0,0x75,0x09,0x48,0x8B};
        static constexpr std::uint8_t projection[] = {
            0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0,0,0,0,0,0,
            0x48,0x85,0xC9,0x74,0x13,0x84,0xD2,0x74};
        static constexpr std::uint8_t hudLayout[] = {
            0x40,0x53,0x48,0x83,0xEC,0x60,0x48,0x8B,0x01,0x48,0x8B,0xD9,
            0x0F,0x29,0x74,0x24,0x50,0x0F,0x29,0x7C,0x24,0x40,0x44,0x0F};
        return Match(kPrepareViewRva, prepare, "xxxx?????xxxxxxxxxxxxxxx", sizeof(prepare)) &&
            Match(kExecuteViewRva, execute, "xxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxx",
                sizeof(execute)) &&
            Match(kFinalizeViewRva, finalize, "xxxxxxxxxxxxxxxxxxxxxxxx", sizeof(finalize)) &&
            Match(kProjectionDispatchRva, projection, "xxxxxxxxxx??????xxxxxxxx",
                sizeof(projection)) &&
            Match(kHudLayoutRva, hudLayout, "xxxxxxxxxxxxxxxxxxxxxxxx", sizeof(hudLayout));
    }

    template<typename T>
    T ReadField(void* base, std::ptrdiff_t offset) noexcept
    {
        if (!base)
            return {};
        __try
        {
            return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(base) + offset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return {};
        }
    }

    void Record(EventKind kind, OpaqueContext* context, std::uint8_t mode,
        OpaqueCamera* explicitCamera = nullptr) noexcept
    {
        const std::uint32_t index = g_eventCount.fetch_add(1, std::memory_order_relaxed);
        if (index >= g_events.size())
            return;
        Event& event = g_events[index];
        event.kind = kind;
        event.mode = mode;
        event.thread = GetCurrentThreadId();
        event.context = context;
        const std::ptrdiff_t cameraOffset = kind == EventKind::Projection
            ? kContextCamera : kPreparedContextCamera;
        event.camera = explicitCamera ? explicitCamera : ReadField<void*>(context, cameraOffset);
        event.descriptor = context ? reinterpret_cast<void*>(
            reinterpret_cast<std::uintptr_t>(context) + kContextDescriptor) : nullptr;
        event.arenaCursor = ReadField<std::uintptr_t>(context, kContextArenaCursor);
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        event.callerRva = caller >= g_moduleBase && caller < g_moduleBase + kImageSize
            ? caller - g_moduleBase : 0;
    }

    Quaternion Normalize(Quaternion value) noexcept;
    Quaternion Multiply(const Quaternion& a, const Quaternion& b) noexcept;
    Vec3 Rotate(const Quaternion& q, const Vec3& value) noexcept;

    void ApplyAlternateEye(OpaqueCamera* camera) noexcept
    {
        const dayz::stereo_state::EyePositions eyePositions =
            dayz::stereo_state::GetEyePositions();
        if (!g_alternateEyeEnabled || !eyePositions.valid)
            return;
        if (!camera)
            return;
        const auto cameraAddress = reinterpret_cast<std::uintptr_t>(camera);
        Vec3 right{};
        Vec3 up{};
        Vec3 forward{};
        Vec3 current{};
        __try
        {
            right = *reinterpret_cast<Vec3*>(cameraAddress + 0x08);
            up = *reinterpret_cast<Vec3*>(cameraAddress + 0x14);
            forward = *reinterpret_cast<Vec3*>(cameraAddress + 0x20);
            current = *reinterpret_cast<Vec3*>(cameraAddress + 0x2C);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }

        const float distanceFromLast = std::fabs(current.x - g_lastStereoTranslation.x) +
            std::fabs(current.y - g_lastStereoTranslation.y) +
            std::fabs(current.z - g_lastStereoTranslation.z);
        if (!g_haveStereoTranslation || g_lastStereoCamera != camera || distanceFromLast > 0.0001f)
            g_baseTranslation = current;

        const unsigned eye = dayz::stereo_state::RenderedEye();
        const float offset = (eye == 0 ? -0.5f : 0.5f) * g_cameraSeparation;
        const auto normalizeAxis = [](const Vec3& axis) {
            const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
            return length > 0.000001f ? Vec3{axis.x / length, axis.y / length,
                axis.z / length} : axis;
        };
        const Vec3 rightUnit = normalizeAxis(right);
        const Vec3 upUnit = normalizeAxis(up);
        const Vec3 forwardUnit = normalizeAxis(forward);
        Vec3 positionalOffset{};
        const dayz::stereo_state::HmdPosition hmdPosition =
            dayz::stereo_state::GetHmdPosition();
        if (hmdPosition.valid)
        {
            if (!g_haveHmdPositionCenter)
            {
                g_hmdPositionCenter = hmdPosition;
                const dayz::stereo_state::HmdOrientation centerOrientation =
                    dayz::stereo_state::GetHmdOrientation();
                if (centerOrientation.valid)
                    g_hmdPositionOrientationCenter = Normalize({centerOrientation.x,
                        centerOrientation.y, centerOrientation.z, centerOrientation.w});
                g_haveHmdPositionCenter = true;
            }
            const float dx = (hmdPosition.x - g_hmdPositionCenter.x) * g_hmdPositionScale;
            const float dy = (hmdPosition.y - g_hmdPositionCenter.y) * g_hmdPositionScale;
            const float dz = (hmdPosition.z - g_hmdPositionCenter.z) * g_hmdPositionScale;
            Vec3 trackingDelta{dx, dy, dz};
            const dayz::stereo_state::HmdOrientation currentOrientation =
                dayz::stereo_state::GetHmdOrientation();
            if (currentOrientation.valid)
            {
                const Quaternion currentHmd = Normalize({currentOrientation.x,
                    currentOrientation.y, currentOrientation.z, currentOrientation.w});
                const Quaternion inverseCenter{-g_hmdPositionOrientationCenter.x,
                    -g_hmdPositionOrientationCenter.y, -g_hmdPositionOrientationCenter.z,
                    g_hmdPositionOrientationCenter.w};
                const Quaternion relativeHmd = Normalize(Multiply(inverseCenter, currentHmd));
                const Quaternion inverseRelative{-relativeHmd.x, -relativeHmd.y,
                    -relativeHmd.z, relativeHmd.w};
                trackingDelta = Rotate(inverseRelative, trackingDelta);
            }
            positionalOffset = {
                rightUnit.x * trackingDelta.x + upUnit.x * trackingDelta.y -
                    forwardUnit.x * trackingDelta.z,
                rightUnit.y * trackingDelta.x + upUnit.y * trackingDelta.y -
                    forwardUnit.y * trackingDelta.z,
                rightUnit.z * trackingDelta.x + upUnit.z * trackingDelta.y -
                    forwardUnit.z * trackingDelta.z};
        }
        const Vec3 translated{
            g_baseTranslation.x + right.x * offset + positionalOffset.x,
            g_baseTranslation.y + right.y * offset + positionalOffset.y,
            g_baseTranslation.z + right.z * offset + positionalOffset.z};
        __try
        {
            *reinterpret_cast<Vec3*>(cameraAddress + 0x2C) = translated;
            const Vec3 verified = *reinterpret_cast<Vec3*>(cameraAddress + 0x2C);
            g_lastStereoTranslation = verified;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        g_lastStereoCamera = camera;
        g_haveStereoTranslation = true;
        const std::uint64_t application = g_stereoApplyCount.fetch_add(1) + 1;
        if (application % 120 == 0)
        {
            char message[320]{};
            sprintf_s(message, "Alternating eye camera verified eye=%u offset=%.6f base=(%.4f,%.4f,%.4f) written=(%.4f,%.4f,%.4f)",
                eye, offset, g_baseTranslation.x, g_baseTranslation.y, g_baseTranslation.z,
                g_lastStereoTranslation.x, g_lastStereoTranslation.y,
                g_lastStereoTranslation.z);
            logging::Info(message);
        }
    }

    Quaternion Normalize(Quaternion value) noexcept
    {
        const float length = std::sqrt(value.x * value.x + value.y * value.y +
            value.z * value.z + value.w * value.w);
        if (length <= 0.000001f)
            return {0.0f, 0.0f, 0.0f, 1.0f};
        const float inverse = 1.0f / length;
        return {value.x * inverse, value.y * inverse, value.z * inverse, value.w * inverse};
    }

    Quaternion Multiply(const Quaternion& a, const Quaternion& b) noexcept
    {
        return {
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }

    Vec3 Rotate(const Quaternion& q, const Vec3& value) noexcept
    {
        const Vec3 u{q.x, q.y, q.z};
        const float dotUv = u.x * value.x + u.y * value.y + u.z * value.z;
        const float dotUu = u.x * u.x + u.y * u.y + u.z * u.z;
        const Vec3 cross{
            u.y * value.z - u.z * value.y,
            u.z * value.x - u.x * value.z,
            u.x * value.y - u.y * value.x};
        return {
            2.0f * dotUv * u.x + (q.w * q.w - dotUu) * value.x + 2.0f * q.w * cross.x,
            2.0f * dotUv * u.y + (q.w * q.w - dotUu) * value.y + 2.0f * q.w * cross.y,
            2.0f * dotUv * u.z + (q.w * q.w - dotUu) * value.z + 2.0f * q.w * cross.z};
    }

    float BasisDistance(const CameraBasis& a, const CameraBasis& b) noexcept
    {
        const auto distance = [](const Vec3& x, const Vec3& y) {
            return std::fabs(x.x - y.x) + std::fabs(x.y - y.y) + std::fabs(x.z - y.z);
        };
        return distance(a.right, b.right) + distance(a.up, b.up) +
            distance(a.forward, b.forward);
    }

    float VectorLength(const Vec3& value) noexcept
    {
        return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    }

    Vec3 Divide(const Vec3& value, float divisor) noexcept
    {
        if (divisor <= 0.000001f)
            return value;
        return {value.x / divisor, value.y / divisor, value.z / divisor};
    }

    Vec3 Scale(const Vec3& value, float factor) noexcept
    {
        return {value.x * factor, value.y * factor, value.z * factor};
    }

    float Dot(const Vec3& a, const Vec3& b) noexcept
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    Vec3 DirectionToOpenXr(const Vec3& direction) noexcept
    {
        const Vec3 right = Divide(g_directionCalibrationBasis.right,
            VectorLength(g_directionCalibrationBasis.right));
        const Vec3 up = Divide(g_directionCalibrationBasis.up,
            VectorLength(g_directionCalibrationBasis.up));
        const Vec3 forward = Divide(g_directionCalibrationBasis.forward,
            VectorLength(g_directionCalibrationBasis.forward));
        const Vec3 unit = Divide(direction, VectorLength(direction));
        return Rotate(g_directionCalibrationXr,
            {Dot(unit, right), Dot(unit, up), -Dot(unit, forward)});
    }

    Vec3 MapOpenXrVectorToCamera(const CameraBasis& base, const Vec3& value) noexcept
    {
        return {
            base.right.x * value.x + base.up.x * value.y - base.forward.x * value.z,
            base.right.y * value.x + base.up.y * value.y - base.forward.y * value.z,
            base.right.z * value.x + base.up.z * value.y - base.forward.z * value.z};
    }

    void ApplyHmdRotationToCamera(OpaqueCamera* camera) noexcept
    {
        if (!g_hmdRotationEnabled)
            return;
        const dayz::stereo_state::HmdOrientation orientation =
            dayz::stereo_state::GetHmdOrientation();
        if (!orientation.valid)
            return;
        if (!camera)
            return;
        const Quaternion current = Normalize({orientation.x, orientation.y, orientation.z,
            orientation.w});
        if (!g_haveHmdCenter)
        {
            const float yaw = std::atan2(
                2.0f * (current.w * current.y + current.x * current.z),
                1.0f - 2.0f * (current.x * current.x + current.y * current.y));
            const float halfYaw = yaw * 0.5f;
            g_hmdCenter = {0.0f, std::sin(halfYaw), 0.0f, std::cos(halfYaw)};
            g_haveHmdCenter = true;
            logging::Info("HMD camera yaw recentered; pitch and roll remain absolute");
        }
        const Quaternion inverseCenter{-g_hmdCenter.x, -g_hmdCenter.y, -g_hmdCenter.z,
            g_hmdCenter.w};
        const Quaternion relative = Normalize(Multiply(inverseCenter, current));
        Quaternion renderRotation = relative;
        const bool guiLook = g_inventoryHmdLookEnabled && IsGuiCursorModeActive();
        if (guiLook)
        {
            if (!g_haveGuiLookCenter)
            {
                g_guiLookHmdCenter = current;
                g_haveGuiLookCenter = true;
                logging::Info("GUI HMD-look center captured; primary camera unlocked");
            }
            const Quaternion inverseGuiCenter{-g_guiLookHmdCenter.x,
                -g_guiLookHmdCenter.y, -g_guiLookHmdCenter.z, g_guiLookHmdCenter.w};
            renderRotation = Normalize(Multiply(inverseGuiCenter, current));
        }
        else
        {
            g_haveGuiLookCenter = false;
        }
        const bool inventoryLook = g_inventoryHmdLookEnabled &&
            g_inventoryPreviewActive.load(std::memory_order_relaxed) &&
            g_haveInventoryPreviewCenter;
        if (inventoryLook)
        {
            const Quaternion inverseInventoryCenter{-g_inventoryPreviewHmdCenter.x,
                -g_inventoryPreviewHmdCenter.y, -g_inventoryPreviewHmdCenter.z,
                g_inventoryPreviewHmdCenter.w};
            renderRotation = Normalize(Multiply(inverseInventoryCenter, current));
        }
        else if (g_hmdNativeAimEnabled && !guiLook)
        {
            // DayZ receives HMD yaw/pitch through its native mouse path so all
            // gameplay systems share them. Only roll remains a render-space
            // transform because a conventional mouse camera has no roll axis.
            const float roll = std::atan2(
                2.0f * (relative.w * relative.z + relative.x * relative.y),
                1.0f - 2.0f * (relative.x * relative.x + relative.z * relative.z));
            const float halfRoll = roll * 0.5f;
            renderRotation = {0.0f, 0.0f, std::sin(halfRoll), std::cos(halfRoll)};
        }
        const auto address = reinterpret_cast<std::uintptr_t>(camera);
        CameraBasis gameBasis{};
        __try
        {
            gameBasis.right = *reinterpret_cast<Vec3*>(address + 0x08);
            gameBasis.up = *reinterpret_cast<Vec3*>(address + 0x14);
            gameBasis.forward = *reinterpret_cast<Vec3*>(address + 0x20);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        if (!g_haveAppliedCameraBasis || g_lastHmdCamera != camera ||
            BasisDistance(gameBasis, g_lastAppliedCameraBasis) > 0.0001f)
            g_baseCameraBasis = gameBasis;
        if (!g_haveDirectionCalibration || g_directionCalibrationCamera != camera)
        {
            g_directionCalibrationCamera = camera;
            g_directionCalibrationBasis = g_baseCameraBasis;
            g_directionCalibrationXr = current;
            g_haveDirectionCalibration = true;
        }

        const Vec3 xrRight = Rotate(renderRotation, {1.0f, 0.0f, 0.0f});
        const Vec3 xrUp = Rotate(renderRotation, {0.0f, 1.0f, 0.0f});
        const Vec3 xrForward = Rotate(renderRotation, {0.0f, 0.0f, -1.0f});
        // FrameBase axes may carry different scale factors. Multiplying the raw
        // scaled basis by the HMD rotation computes S*R and introduces shear
        // (the image becomes a diamond). Rotate an orthogonal unit basis first,
        // then restore each DayZ axis scale: R*S.
        const float rightScale = VectorLength(g_baseCameraBasis.right);
        const float upScale = VectorLength(g_baseCameraBasis.up);
        const float forwardScale = VectorLength(g_baseCameraBasis.forward);
        const CameraBasis unitBasis{
            Divide(g_baseCameraBasis.right, rightScale),
            Divide(g_baseCameraBasis.up, upScale),
            Divide(g_baseCameraBasis.forward, forwardScale)};
        const CameraBasis rotated{
            Scale(MapOpenXrVectorToCamera(unitBasis, xrRight), rightScale),
            Scale(MapOpenXrVectorToCamera(unitBasis, xrUp), upScale),
            Scale(MapOpenXrVectorToCamera(unitBasis, xrForward), forwardScale)};
        const Vec3 nativeDirection = DirectionToOpenXr(g_baseCameraBasis.forward);
        const Vec3 renderDirection = DirectionToOpenXr(rotated.forward);
        dayz::stereo_state::UpdateCameraDirections(nativeDirection.x, nativeDirection.y,
            nativeDirection.z, renderDirection.x, renderDirection.y, renderDirection.z);
        __try
        {
            *reinterpret_cast<Vec3*>(address + 0x08) = rotated.right;
            *reinterpret_cast<Vec3*>(address + 0x14) = rotated.up;
            *reinterpret_cast<Vec3*>(address + 0x20) = rotated.forward;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        g_lastHmdCamera = camera;
        g_lastAppliedCameraBasis = rotated;
        g_haveAppliedCameraBasis = true;
        const std::uint64_t application = g_hmdApplyCount.fetch_add(1) + 1;
        if (application % 120 == 0)
        {
            float activeTransform[12]{};
            bool haveActiveTransform{};
            if (kCameraManagerRva && kGetActiveCameraStateRva)
            {
                __try
                {
                    using GetActiveCameraStateFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
                    const auto getActive = reinterpret_cast<GetActiveCameraStateFn>(
                        g_moduleBase + kGetActiveCameraStateRva);
                    const std::uintptr_t state = getActive(g_moduleBase + kCameraManagerRva);
                    if (state)
                    {
                        const float* transform = reinterpret_cast<const float*>(state + 24);
                        for (unsigned index = 0; index < 12; ++index)
                            activeTransform[index] = transform[index];
                        haveActiveTransform = true;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
            char message[1024]{};
            sprintf_s(message, "direction probe hmd_relative_q=(%.4f,%.4f,%.4f,%.4f) dayz_native_xr=(%.4f,%.4f,%.4f) dayz_render_xr=(%.4f,%.4f,%.4f) basis_delta=%.5f active_state_3x4=%s[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]",
                relative.x, relative.y, relative.z, relative.w,
                nativeDirection.x, nativeDirection.y, nativeDirection.z,
                renderDirection.x, renderDirection.y, renderDirection.z,
                BasisDistance(g_baseCameraBasis, rotated),
                haveActiveTransform ? "" : "unavailable",
                activeTransform[0], activeTransform[1], activeTransform[2], activeTransform[3],
                activeTransform[4], activeTransform[5], activeTransform[6], activeTransform[7],
                activeTransform[8], activeTransform[9], activeTransform[10], activeTransform[11]);
            logging::Info(message);
        }
    }

    void ResetInventoryPreviewAnchor() noexcept
    {
        g_inventoryPreviewActive.store(false, std::memory_order_relaxed);
        g_inventoryPreviewOrdinal = 0;
        g_inventoryPreviewCamera = nullptr;
        g_haveInventoryPreviewCenter = false;
        g_haveInventoryPreviewBasis = false;
    }

    void ApplyInventoryPreviewRotation(OpaqueCamera* camera) noexcept
    {
        if (!g_hmdRotationEnabled || !camera)
            return;
        const dayz::stereo_state::HmdOrientation orientation =
            dayz::stereo_state::GetHmdOrientation();
        if (!orientation.valid)
            return;
        const Quaternion current = Normalize({orientation.x, orientation.y, orientation.z,
            orientation.w});
        if (!g_haveInventoryPreviewCenter)
        {
            g_inventoryPreviewHmdCenter = current;
            g_haveInventoryPreviewCenter = true;
            logging::Info("Inventory preview HMD anchor captured");
        }
        const Quaternion inverseCenter{-g_inventoryPreviewHmdCenter.x,
            -g_inventoryPreviewHmdCenter.y, -g_inventoryPreviewHmdCenter.z,
            g_inventoryPreviewHmdCenter.w};
        const Quaternion relative = Normalize(Multiply(inverseCenter, current));
        const float rotationScale = (std::clamp)(g_inventoryPreviewRotationScale, -2.0f,
            2.0f);
        const Quaternion scaledRelative = Normalize({relative.x * rotationScale,
            relative.y * rotationScale, relative.z * rotationScale,
            1.0f + (relative.w - 1.0f) * rotationScale});
        const Quaternion menuLockedRotation{-scaledRelative.x, -scaledRelative.y,
            -scaledRelative.z, scaledRelative.w};

        const auto address = reinterpret_cast<std::uintptr_t>(camera);
        CameraBasis gameBasis{};
        __try
        {
            gameBasis.right = *reinterpret_cast<Vec3*>(address + 0x08);
            gameBasis.up = *reinterpret_cast<Vec3*>(address + 0x14);
            gameBasis.forward = *reinterpret_cast<Vec3*>(address + 0x20);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        if (!g_haveInventoryPreviewBasis || g_inventoryPreviewCamera != camera ||
            BasisDistance(gameBasis, g_inventoryPreviewLastBasis) > 0.0001f)
            g_inventoryPreviewBaseBasis = gameBasis;

        const Vec3 xrRight = Rotate(menuLockedRotation, {1.0f, 0.0f, 0.0f});
        const Vec3 xrUp = Rotate(menuLockedRotation, {0.0f, 1.0f, 0.0f});
        const Vec3 xrForward = Rotate(menuLockedRotation, {0.0f, 0.0f, -1.0f});
        const float rightScale = VectorLength(g_inventoryPreviewBaseBasis.right);
        const float upScale = VectorLength(g_inventoryPreviewBaseBasis.up);
        const float forwardScale = VectorLength(g_inventoryPreviewBaseBasis.forward);
        const CameraBasis unitBasis{
            Divide(g_inventoryPreviewBaseBasis.right, rightScale),
            Divide(g_inventoryPreviewBaseBasis.up, upScale),
            Divide(g_inventoryPreviewBaseBasis.forward, forwardScale)};
        const CameraBasis rotated{
            Scale(MapOpenXrVectorToCamera(unitBasis, xrRight), rightScale),
            Scale(MapOpenXrVectorToCamera(unitBasis, xrUp), upScale),
            Scale(MapOpenXrVectorToCamera(unitBasis, xrForward), forwardScale)};
        __try
        {
            *reinterpret_cast<Vec3*>(address + 0x08) = rotated.right;
            *reinterpret_cast<Vec3*>(address + 0x14) = rotated.up;
            *reinterpret_cast<Vec3*>(address + 0x20) = rotated.forward;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        g_inventoryPreviewCamera = camera;
        g_inventoryPreviewLastBasis = rotated;
        g_haveInventoryPreviewBasis = true;
    }

    void HideInventoryPlayerPreview(OpaqueCamera* camera) noexcept
    {
        if (!camera)
            return;
        const auto address = reinterpret_cast<std::uintptr_t>(camera);
        __try
        {
            Vec3 position = *reinterpret_cast<Vec3*>(address + 0x2C);
            position.y += 1000000.0f;
            *reinterpret_cast<Vec3*>(address + 0x2C) = position;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    std::uintptr_t __fastcall HookedFrameRefresh(OpaqueCamera* camera, void* engine,
        std::uint8_t rebuild)
    {
        // This is deliberately done before DayZ derives any view, inverse-view,
        // culling or view-projection matrices. Applying it in the virtual basis
        // getter is too late: several consumers have already cached the old pose.
        const bool primaryProjectionCamera = g_projectionRefreshDepth != 0 &&
            camera == g_projectionContextCamera;
        if (primaryProjectionCamera)
        {
            ApplyHmdRotationToCamera(camera);
            ApplyAlternateEye(camera);
        }
        const std::uintptr_t result = g_frameRefresh(camera, engine, rebuild);
        return result;
    }

    float* __fastcall HookedCameraRefresh(OpaqueCamera* camera, void* output)
    {
        // The same primary FrameBase getter is consumed by gameplay aiming and
        // first-person attachment preparation outside ProjectionDispatch. Keep
        // those consumers on the HMD pose too, but never touch auxiliary object
        // cameras (inventory, hotbar, world items, mirrors, etc.).
        const bool primaryProjectionCamera = g_projectionRefreshDepth != 0 &&
            camera == g_projectionContextCamera;
        const bool knownPlayerCamera = camera && camera == g_lastHmdCamera;
        const bool applyHmdRotation = primaryProjectionCamera || knownPlayerCamera;
        if (applyHmdRotation)
            ApplyHmdRotationToCamera(camera);

        float* result = g_cameraRefresh(camera, output);

        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const std::uintptr_t callerRva = caller >= g_moduleBase &&
            caller < g_moduleBase + kImageSize ? caller - g_moduleBase : 0;
        bool known{};
        for (auto& recorded : g_cameraRefreshCallers)
        {
            if (recorded.load(std::memory_order_relaxed) == callerRva + 1)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            for (auto& recorded : g_cameraRefreshCallers)
            {
                std::uintptr_t empty{};
                if (recorded.compare_exchange_strong(empty, callerRva + 1))
                {
                    std::ostringstream message;
                    message << "Camera refresh caller=DayZ+0x" << std::hex << callerRva
                        << " self=" << camera << " projectionCamera="
                        << g_projectionContextCamera << std::dec
                        << " projectionDepth=" << g_projectionRefreshDepth;
                    logging::Info(message.str());
                    break;
                }
            }
        }
        return result;
    }

    void* ReadCameraRefreshTarget(OpaqueCamera* camera) noexcept
    {
        __try
        {
            void** table = *reinterpret_cast<void***>(camera);
            return table ? table[72 / sizeof(void*)] : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void EnsureCameraRefreshHook(OpaqueContext* context) noexcept
    {
        if (!g_hmdRotationEnabled || g_cameraRefreshHookAttempted.load())
            return;
        OpaqueCamera* camera = ReadField<OpaqueCamera*>(context, kContextCamera);
        if (!camera)
            return;
        void* target = ReadCameraRefreshTarget(camera);
        if (!target || g_cameraRefreshHookAttempted.exchange(true))
            return;
        const MH_STATUS created = MH_CreateHook(target, HookedCameraRefresh,
            reinterpret_cast<void**>(&g_cameraRefresh));
        const MH_STATUS enabled = created == MH_OK ? MH_EnableHook(target) : created;
        if (created != MH_OK || (enabled != MH_OK && enabled != MH_ERROR_ENABLED))
        {
            logging::Error("HMD camera refresh hook installation failed");
            return;
        }
        const auto address = reinterpret_cast<std::uintptr_t>(target);
        std::ostringstream message;
        message << "HMD camera refresh hook active at DayZ+0x" << std::hex
            << (address >= g_moduleBase && address < g_moduleBase + kImageSize
                ? address - g_moduleBase : 0);
        logging::Info(message.str());
    }

    void __fastcall HookedPrepareView(OpaqueEngine* engine, OpaqueContext* context,
        std::uint8_t mode, OpaqueCamera* camera)
    {
        // Projection identifies the primary FrameBase, but DayZ prepares that
        // same object again before copying it into the render context. Reapply
        // HMD here in case game-side camera state was refreshed in between.
        if (g_hmdRotationEnabled && camera && camera == g_lastHmdCamera)
            ApplyHmdRotationToCamera(camera);
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const std::uintptr_t callerRva = caller >= g_moduleBase &&
            caller < g_moduleBase + kImageSize ? caller - g_moduleBase : 0;
        if (kInventoryPreviewPrepareCallerRva &&
            callerRva == kInventoryPreviewPrepareCallerRva && IsGuiCursorModeActive())
        {
            g_inventoryPreviewActive.store(true, std::memory_order_relaxed);
            // DayZ submits three auxiliary inventory views in a stable order.
            // The first and largest one is the character preview. Later views
            // are item previews already captured into the world-locked GUI and
            // must not receive an additional camera compensation.
            if (g_inventoryPreviewOrdinal++ == 0)
            {
                if (g_inventoryPlayerPreviewVisible)
                    ApplyInventoryPreviewRotation(camera);
                else
                    HideInventoryPlayerPreview(camera);
            }
        }
        Record(EventKind::Prepare, context, mode, camera);
        g_prepareView(engine, context, mode, camera);
    }

    void* __fastcall HookedExecuteView(OpaqueEngine* engine, OpaqueContext* context,
        std::uint8_t mode)
    {
        Record(EventKind::Execute, context, mode);
        return g_executeView(engine, context, mode);
    }

    std::uintptr_t __fastcall HookedFinalizeView(OpaqueEngine* engine, OpaqueContext* context,
        std::uint8_t mode)
    {
        Record(EventKind::Finalize, context, mode);
        return g_finalizeView(engine, context, mode);
    }

    std::uintptr_t __fastcall HookedProjectionDispatch(OpaqueContext* context, std::uint8_t mode)
    {
        ApplyProfileFovOverride();
        ApplyActiveCameraFovOverride();
        EnsureCameraRefreshHook(context);
        ApplyAlternateEye(ReadField<OpaqueCamera*>(context, kContextCamera));
        Record(EventKind::Projection, context, mode);
        OpaqueCamera* previousProjectionCamera = g_projectionContextCamera;
        g_projectionContextCamera = ReadField<OpaqueCamera*>(context, kContextCamera);
        ++g_projectionRefreshDepth;
        const std::uintptr_t result = g_projectionDispatch(context, mode);
        --g_projectionRefreshDepth;
        // Deferred rendering still consumes this primary FrameBase after the
        // dispatch returns. Auxiliary object cameras are separate instances
        // and are excluded by HookedCameraRefresh.
        g_projectionContextCamera = previousProjectionCamera;
        return result;
    }

    char __fastcall HookedHudLayout(void* renderer)
    {
        const char result = g_hudLayout(renderer);
        // The native layout routine recalculates and overwrites IGUIScale, so apply both
        // the global value and its dependent safe-area rectangle only after it returns.
        if (g_overrideHudScale)
            WriteHudScale();
        ApplyHudSafeArea(renderer);
        return result;
    }

    void __fastcall HookedDynamicBlur(std::int64_t effect, unsigned int view,
        std::int64_t input, std::int64_t output, int a5, int a6, int a7,
        std::int64_t parameters)
    {
        float* blurAmount{};
        float savedAmount{};
        if (!g_inventoryBlurEnabled &&
            g_inventoryPreviewActive.load(std::memory_order_relaxed) && parameters &&
            kDynamicBlurParameterIndexRva)
        {
            __try
            {
                const auto parameterTable = *reinterpret_cast<std::uintptr_t*>(parameters + 32);
                const auto parameterIndex = *reinterpret_cast<std::uint32_t*>(
                    g_moduleBase + kDynamicBlurParameterIndexRva);
                if (parameterTable)
                    blurAmount = *reinterpret_cast<float**>(parameterTable +
                        sizeof(void*) * parameterIndex);
                if (blurAmount)
                {
                    savedAmount = *blurAmount;
                    *blurAmount = 0.0f;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                blurAmount = nullptr;
            }
        }
        g_dynamicBlur(effect, view, input, output, a5, a6, a7, parameters);
        if (blurAmount)
        {
            __try
            {
                *blurAmount = savedAmount;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
    }

    template<typename T>
    bool AddHook(std::uintptr_t rva, void* detour, T& original) noexcept
    {
        return MH_CreateHook(reinterpret_cast<void*>(g_moduleBase + rva), detour,
            reinterpret_cast<void**>(&original)) == MH_OK;
    }

    const char* EventName(EventKind kind) noexcept
    {
        switch (kind)
        {
        case EventKind::Projection: return "projection";
        case EventKind::Prepare: return "prepare";
        case EventKind::Execute: return "execute";
        case EventKind::Finalize: return "finalize";
        }
        return "unknown";
    }

    void DescribeResource(ID3D11View* view, ApiEvent& event) noexcept
    {
        if (!view)
            return;
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (resource && SUCCEEDED(resource.As(&texture)))
        {
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            event.width = description.Width;
            event.height = description.Height;
            event.format = static_cast<std::uint32_t>(description.Format);
            event.samples = description.SampleDesc.Count;
        }
    }

    ApiEvent* BeginApiEvent(ApiKind kind) noexcept
    {
        if (!g_captureApi.load(std::memory_order_relaxed))
            return nullptr;
        const std::uint32_t index = g_apiEventCount.fetch_add(1, std::memory_order_relaxed);
        if (index >= g_apiEvents.size())
            return nullptr;
        ApiEvent& event = g_apiEvents[index];
        event = {};
        event.kind = kind;
        event.thread = GetCurrentThreadId();
        return &event;
    }

    void RecordHudCompositeCandidate(ID3D11DeviceContext* context, ApiKind kind,
        UINT elementCount, std::uintptr_t caller) noexcept
    {
        if (!g_captureApi.load(std::memory_order_relaxed) ||
            g_hudSafeWidth <= 0.0f || g_hudSafeHeight <= 0.0f)
            return;

        ID3D11ShaderResourceView* resources[16]{};
        context->PSGetShaderResources(0, static_cast<UINT>(std::size(resources)), resources);
        ID3D11ShaderResourceView* candidate{};
        ApiEvent sourceDescription{};
        const auto expectedWidth = static_cast<std::uint32_t>(std::lround(g_hudSafeWidth));
        const auto expectedHeight = static_cast<std::uint32_t>(std::lround(g_hudSafeHeight));
        for (ID3D11ShaderResourceView* resource : resources)
        {
            if (!resource)
                continue;
            ApiEvent description{};
            DescribeResource(resource, description);
            if (!candidate && description.width == expectedWidth &&
                description.height == expectedHeight)
            {
                candidate = resource;
                sourceDescription = description;
            }
        }

        ID3D11RenderTargetView* target{};
        context->OMGetRenderTargets(1, &target, nullptr);
        ApiEvent targetDescription{};
        DescribeResource(target, targetDescription);
        if (candidate && targetDescription.width >= expectedWidth &&
            targetDescription.height >= expectedHeight)
        {
            if (ApiEvent* event = BeginApiEvent(kind))
            {
                event->object0 = target;
                event->object1 = candidate;
                event->width = sourceDescription.width;
                event->height = sourceDescription.height;
                event->format = sourceDescription.format;
                event->samples = sourceDescription.samples;
                event->targetWidth = targetDescription.width;
                event->targetHeight = targetDescription.height;
                event->elementCount = elementCount;
                event->callerRva = caller >= g_moduleBase && caller < g_moduleBase + kImageSize
                    ? caller - g_moduleBase : 0;
            }
        }
        if (target)
            target->Release();
        for (ID3D11ShaderResourceView* resource : resources)
            if (resource)
                resource->Release();
    }

    void RecordAlphaDrawState(ID3D11DeviceContext* context, ApiKind kind, UINT elementCount,
        std::uintptr_t caller) noexcept
    {
        if (!g_captureApi.load(std::memory_order_relaxed))
            return;

        ID3D11RenderTargetView* target{};
        context->OMGetRenderTargets(1, &target, nullptr);
        ApiEvent targetDescription{};
        DescribeResource(target, targetDescription);
        if (target)
            target->Release();
        if (targetDescription.width < 1000 || targetDescription.height < 1000)
            return;

        ID3D11BlendState* blendState{};
        FLOAT blendFactor[4]{};
        UINT sampleMask{};
        context->OMGetBlendState(&blendState, blendFactor, &sampleMask);
        D3D11_BLEND_DESC blendDescription{};
        if (blendState)
            blendState->GetDesc(&blendDescription);
        if (!blendState || !blendDescription.RenderTarget[0].BlendEnable)
        {
            if (blendState)
                blendState->Release();
            return;
        }

        ID3D11DepthStencilState* depthState{};
        UINT stencilReference{};
        context->OMGetDepthStencilState(&depthState, &stencilReference);
        D3D11_DEPTH_STENCIL_DESC depthDescription{};
        if (depthState)
            depthState->GetDesc(&depthDescription);
        ID3D11PixelShader* pixelShader{};
        ID3D11VertexShader* vertexShader{};
        context->PSGetShader(&pixelShader, nullptr, nullptr);
        context->VSGetShader(&vertexShader, nullptr, nullptr);

        const std::uint32_t existingCount = (std::min)(
            g_drawStateEventCount.load(std::memory_order_relaxed),
            static_cast<std::uint32_t>(g_drawStateEvents.size()));
        bool duplicate{};
        for (std::uint32_t index = 0; index < existingCount; ++index)
        {
            const DrawStateEvent& existing = g_drawStateEvents[index];
            if (existing.pixelShader == pixelShader && existing.vertexShader == vertexShader &&
                existing.blendState == blendState && existing.depthState == depthState &&
                existing.targetFormat == targetDescription.format)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
        {
            const std::uint32_t index = g_drawStateEventCount.fetch_add(1,
                std::memory_order_relaxed);
            if (index < g_drawStateEvents.size())
            {
                DrawStateEvent& event = g_drawStateEvents[index];
                event.kind = kind;
                event.thread = GetCurrentThreadId();
                event.pixelShader = pixelShader;
                event.vertexShader = vertexShader;
                event.blendState = blendState;
                event.depthState = depthState;
                event.targetWidth = targetDescription.width;
                event.targetHeight = targetDescription.height;
                event.targetFormat = targetDescription.format;
                event.elementCount = elementCount;
                event.callerRva = caller >= g_moduleBase && caller < g_moduleBase + kImageSize
                    ? caller - g_moduleBase : 0;
                void* frames[12]{};
                const USHORT frameCount = CaptureStackBackTrace(1,
                    static_cast<DWORD>(std::size(frames)), frames, nullptr);
                for (USHORT frameIndex = 0; frameIndex < frameCount; ++frameIndex)
                {
                    const auto address = reinterpret_cast<std::uintptr_t>(frames[frameIndex]);
                    if (address < g_moduleBase || address >= g_moduleBase + kImageSize)
                        continue;
                    const std::uintptr_t rva = address - g_moduleBase;
                    if (rva >= 0x0025F000 && rva < 0x00261000)
                        continue;
                    event.parentCallerRva = rva;
                    break;
                }
                UINT viewportCount = 1;
                D3D11_VIEWPORT viewport{};
                context->RSGetViewports(&viewportCount, &viewport);
                if (viewportCount)
                {
                    event.viewportX = viewport.TopLeftX;
                    event.viewportY = viewport.TopLeftY;
                    event.viewportWidth = viewport.Width;
                    event.viewportHeight = viewport.Height;
                }
                event.depthEnabled = depthState && depthDescription.DepthEnable;
                event.depthWrite = depthState &&
                    depthDescription.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
            }
        }
        if (pixelShader)
            pixelShader->Release();
        if (vertexShader)
            vertexShader->Release();
        if (depthState)
            depthState->Release();
        blendState->Release();
    }

    bool IsHudLayerDraw(ID3D11DeviceContext* context, std::uintptr_t caller,
        ID3D11RenderTargetView* target, bool guiCapture) noexcept
    {
        const bool explicitComposite =
            g_hudCompositeWidth > 0.0f && g_hudCompositeHeight > 0.0f;
        const bool scaledComposite = g_overrideHudScale && g_hudScale < 0.999f;
        if ((!guiCapture && !explicitComposite && !scaledComposite) || !target ||
            (caller != g_moduleBase + 0x002601C2 && caller != g_moduleBase + 0x0026038E))
            return false;
        ApiEvent targetDescription{};
        DescribeResource(target, targetDescription);
        if (targetDescription.width < 1000 || targetDescription.height < 1000)
            return false;
        ID3D11BlendState* blendState{};
        FLOAT factor[4]{};
        UINT mask{};
        context->OMGetBlendState(&blendState, factor, &mask);
        D3D11_BLEND_DESC blendDescription{};
        if (blendState)
            blendState->GetDesc(&blendDescription);
        const bool alphaBlended = blendState && blendDescription.RenderTarget[0].BlendEnable;
        if (blendState)
            blendState->Release();
        if (!alphaBlended)
            return false;
        ID3D11DepthStencilState* depthState{};
        UINT stencil{};
        context->OMGetDepthStencilState(&depthState, &stencil);
        D3D11_DEPTH_STENCIL_DESC depthDescription{};
        if (depthState)
            depthState->GetDesc(&depthDescription);
        const bool depthDisabled = !depthState || !depthDescription.DepthEnable;
        if (depthState)
            depthState->Release();
        return depthDisabled;
    }

    bool IsInventoryLayerDraw(ID3D11DeviceContext* context, std::uintptr_t caller,
        ID3D11RenderTargetView* target, bool guiCapture) noexcept
    {
        // The same indexed GUI path is used for both inventory objects and the 3D
        // hotbar icons. Capture it into the scaled HUD only while the GUI cursor is
        // genuinely hidden. Raw cursor state excludes the inventory immediately,
        // including the first frame before the debounced GUI-quad state activates.
        const bool explicitComposite =
            g_hudCompositeWidth > 0.0f && g_hudCompositeHeight > 0.0f;
        const bool scaledGameplayHud = g_overrideHudScale && g_hudScale < 0.999f &&
            !RawGuiCursorModeActive();
        if ((!guiCapture && !explicitComposite && !scaledGameplayHud) || !target ||
            caller != g_moduleBase + 0x002600DD)
            return false;

        ApiEvent targetDescription{};
        DescribeResource(target, targetDescription);
        if (targetDescription.width < 1000 || targetDescription.height < 1000)
            return false;

        ID3D11RasterizerState* rasterizerState{};
        context->RSGetState(&rasterizerState);
        D3D11_RASTERIZER_DESC rasterizerDescription{};
        if (rasterizerState)
            rasterizerState->GetDesc(&rasterizerDescription);
        if (rasterizerState)
            rasterizerState->Release();
        if (!rasterizerDescription.ScissorEnable)
            return false;

        UINT scissorCount = 1;
        D3D11_RECT scissor{};
        context->RSGetScissorRects(&scissorCount, &scissor);
        if (!scissorCount)
            return false;
        const LONG width = scissor.right - scissor.left;
        const LONG height = scissor.bottom - scissor.top;
        return width >= 16 && height >= 16 &&
            (width < static_cast<LONG>(targetDescription.width) ||
             height < static_cast<LONG>(targetDescription.height));
    }

    bool EnsureHudLayer(const D3D11_TEXTURE2D_DESC& source) noexcept
    {
        if (!g_d3dDevice)
            return false;
        std::scoped_lock lock(g_hudLayerMutex);
        if (g_hudLayerTarget && g_hudLayerWidth == source.Width &&
            g_hudLayerHeight == source.Height && g_hudLayerSamples.Count == source.SampleDesc.Count &&
            g_hudLayerSamples.Quality == source.SampleDesc.Quality)
            return true;
        g_hudLayerView.Reset();
        g_hudLayerResolved.Reset();
        g_hudLayerTarget.Reset();
        g_hudLayerTexture.Reset();
        D3D11_TEXTURE2D_DESC description{};
        description.Width = source.Width;
        description.Height = source.Height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc = source.SampleDesc;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_d3dDevice->CreateTexture2D(&description, nullptr, &g_hudLayerTexture)) ||
            FAILED(g_d3dDevice->CreateRenderTargetView(g_hudLayerTexture.Get(), nullptr,
                &g_hudLayerTarget)))
            return false;
        description.SampleDesc = {1, 0};
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_d3dDevice->CreateTexture2D(&description, nullptr, &g_hudLayerResolved)) ||
            FAILED(g_d3dDevice->CreateShaderResourceView(g_hudLayerResolved.Get(), nullptr,
                &g_hudLayerView)))
            return false;
        g_hudLayerWidth = source.Width;
        g_hudLayerHeight = source.Height;
        g_guiNativeWidth = source.Width;
        g_guiNativeHeight = source.Height;
        g_hudLayerSamples = source.SampleDesc;
        g_hudLayerNeedsClear = true;
        g_hudLayerLogged = false;
        return true;
    }

    bool EnsureGuiLayer(const D3D11_TEXTURE2D_DESC& source) noexcept
    {
        if (!g_d3dDevice)
            return false;
        std::scoped_lock lock(g_guiLayerMutex);
        if (g_guiLayerTarget && g_guiLayerWidth == source.Width &&
            g_guiLayerHeight == source.Height && g_guiLayerSamples.Count ==
            source.SampleDesc.Count && g_guiLayerSamples.Quality == source.SampleDesc.Quality)
            return true;
        if (g_guiLayerTarget && static_cast<std::uint64_t>(source.Width) * source.Height <
            static_cast<std::uint64_t>(g_guiLayerWidth) * g_guiLayerHeight)
            return false;

        g_guiLayerView.Reset();
        g_guiLayerResolved.Reset();
        g_guiLayerTarget.Reset();
        g_guiLayerTexture.Reset();
        D3D11_TEXTURE2D_DESC description{};
        description.Width = source.Width;
        description.Height = source.Height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc = source.SampleDesc;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_d3dDevice->CreateTexture2D(&description, nullptr, &g_guiLayerTexture)) ||
            FAILED(g_d3dDevice->CreateRenderTargetView(g_guiLayerTexture.Get(), nullptr,
                &g_guiLayerTarget)))
            return false;
        description.SampleDesc = {1, 0};
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_d3dDevice->CreateTexture2D(&description, nullptr, &g_guiLayerResolved)) ||
            FAILED(g_d3dDevice->CreateShaderResourceView(g_guiLayerResolved.Get(), nullptr,
                &g_guiLayerView)))
            return false;
        g_guiLayerWidth = source.Width;
        g_guiLayerHeight = source.Height;
        g_guiNativeWidth = source.Width;
        g_guiNativeHeight = source.Height;
        g_guiLayerSamples = source.SampleDesc;
        g_guiLayerNeedsClear = true;
        g_guiLayerLogged = false;
        return true;
    }

    struct HudRedirectState
    {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
    };

    bool BeginHudLayerDraw(ID3D11DeviceContext* context, std::uintptr_t caller,
        bool indexed, HudRedirectState& original) noexcept
    {
        ID3D11RenderTargetView* target{};
        ID3D11DepthStencilView* depth{};
        context->OMGetRenderTargets(1, &target, &depth);
        original.target.Attach(target);
        original.depth.Attach(depth);
        const bool guiCandidate = indexed ? caller == g_moduleBase + 0x002600DD :
            (caller == g_moduleBase + 0x002601C2 || caller == g_moduleBase + 0x0026038E);
        const bool guiCapture = g_guiQuadEnabled && guiCandidate && IsGuiCursorModeActive();
        if (indexed ? !IsInventoryLayerDraw(context, caller, target, guiCapture) :
                !IsHudLayerDraw(context, caller, target, guiCapture))
            return false;
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        target->GetResource(&resource);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (!resource || FAILED(resource.As(&texture)))
            return false;
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (guiCapture)
        {
            if (!EnsureGuiLayer(description))
                return false;
            if (g_guiLayerNeedsClear.exchange(false))
            {
                constexpr FLOAT transparent[4]{};
                g_clearRtv(context, g_guiLayerTarget.Get(), transparent);
            }
            ID3D11RenderTargetView* guiTarget = g_guiLayerTarget.Get();
            g_omSetRenderTargets(context, 1, &guiTarget,
                indexed ? original.depth.Get() : nullptr);
            g_guiLayerDirty = true;
            g_guiLayerCapturedPresent.store(g_presentCount.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            if (!g_guiLayerLogged.exchange(true))
            {
                std::ostringstream message;
                message << "World-locked GUI capture active: " << description.Width << 'x'
                    << description.Height << " samples=" << description.SampleDesc.Count;
                logging::Info(message.str());
            }
            return true;
        }

        if (!EnsureHudLayer(description))
            return false;
        if (g_hudLayerNeedsClear.exchange(false))
        {
            constexpr FLOAT transparent[4]{};
            g_clearRtv(context, g_hudLayerTarget.Get(), transparent);
        }
        ID3D11RenderTargetView* hudTarget = g_hudLayerTarget.Get();
        g_omSetRenderTargets(context, 1, &hudTarget,
            indexed ? original.depth.Get() : nullptr);
        g_hudLayerDirty = true;
        if (indexed && !g_inventoryLayerLogged.exchange(true))
            logging::Info(guiCapture ?
                "Inventory DrawIndexed redirected into world-locked GUI layer" :
                "3D hotbar DrawIndexed redirected into scaled HUD layer");
        if (!g_hudLayerLogged.exchange(true))
        {
            std::ostringstream message;
            message << "Native HUD layer active: " << description.Width << 'x'
                << description.Height << " samples=" << description.SampleDesc.Count
                << " automatic_scale=" << (g_overrideHudScale ? g_hudScale : 1.0f);
            logging::Info(message.str());
        }
        return true;
    }

    void STDMETHODCALLTYPE HookedDrawIndexed(ID3D11DeviceContext* self, UINT indexCount,
        UINT startIndex, INT baseVertex)
    {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        RecordHudCompositeCandidate(self, ApiKind::DrawIndexed, indexCount, caller);
        RecordAlphaDrawState(self, ApiKind::DrawIndexed, indexCount, caller);
        HudRedirectState originalTargets{};
        const bool redirected = BeginHudLayerDraw(self, caller, true, originalTargets);
        g_drawIndexed(self, indexCount, startIndex, baseVertex);
        if (redirected)
        {
            ID3D11RenderTargetView* target = originalTargets.target.Get();
            g_omSetRenderTargets(self, target ? 1u : 0u, target ? &target : nullptr,
                originalTargets.depth.Get());
        }
    }

    void STDMETHODCALLTYPE HookedDraw(ID3D11DeviceContext* self, UINT vertexCount,
        UINT startVertex)
    {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        RecordHudCompositeCandidate(self, ApiKind::Draw, vertexCount, caller);
        RecordAlphaDrawState(self, ApiKind::Draw, vertexCount, caller);
        HudRedirectState originalTargets{};
        const bool redirected = BeginHudLayerDraw(self, caller, false, originalTargets);
        g_draw(self, vertexCount, startVertex);
        if (redirected)
        {
            ID3D11RenderTargetView* target = originalTargets.target.Get();
            g_omSetRenderTargets(self, target ? 1u : 0u, target ? &target : nullptr,
                originalTargets.depth.Get());
        }
    }

    void STDMETHODCALLTYPE HookedOmSetRenderTargets(ID3D11DeviceContext* self, UINT count,
        ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth)
    {
        if (ApiEvent* event = BeginApiEvent(ApiKind::Targets))
        {
            event->object0 = count && targets ? targets[0] : nullptr;
            event->object1 = depth;
            DescribeResource(count && targets ? targets[0] : nullptr, *event);
        }
        g_omSetRenderTargets(self, count, targets, depth);
    }

    void STDMETHODCALLTYPE HookedRsSetViewports(ID3D11DeviceContext* self, UINT count,
        const D3D11_VIEWPORT* viewports)
    {
        if (ApiEvent* event = BeginApiEvent(ApiKind::Viewport); event && count && viewports)
        {
            event->width = static_cast<std::uint32_t>(viewports[0].Width);
            event->height = static_cast<std::uint32_t>(viewports[0].Height);
        }
        g_rsSetViewports(self, count, viewports);
    }

    void STDMETHODCALLTYPE HookedClearRtv(ID3D11DeviceContext* self,
        ID3D11RenderTargetView* target, const FLOAT color[4])
    {
        if (ApiEvent* event = BeginApiEvent(ApiKind::ClearColor))
        {
            event->object0 = target;
            DescribeResource(target, *event);
        }
        g_clearRtv(self, target, color);
    }

    void STDMETHODCALLTYPE HookedClearDsv(ID3D11DeviceContext* self,
        ID3D11DepthStencilView* depth, UINT flags, FLOAT value, UINT8 stencil)
    {
        if (ApiEvent* event = BeginApiEvent(ApiKind::ClearDepth))
        {
            event->object0 = depth;
            DescribeResource(depth, *event);
        }
        g_clearDsv(self, depth, flags, value, stencil);
    }

    void STDMETHODCALLTYPE HookedCopyResource(ID3D11DeviceContext* self, ID3D11Resource* target,
        ID3D11Resource* source)
    {
        if (ApiEvent* event = BeginApiEvent(ApiKind::Copy))
        {
            event->object0 = target;
            event->object1 = source;
        }
        g_copyResource(self, target, source);
    }

    void STDMETHODCALLTYPE HookedResolve(ID3D11DeviceContext* self, ID3D11Resource* target,
        UINT targetSubresource, ID3D11Resource* source, UINT sourceSubresource,
        DXGI_FORMAT format)
    {
        if (ApiEvent* event = BeginApiEvent(ApiKind::Resolve))
        {
            event->object0 = target;
            event->object1 = source;
            event->format = static_cast<std::uint32_t>(format);
        }
        g_resolve(self, target, targetSubresource, source, sourceSubresource, format);
    }

    bool InstallD3DHooks() noexcept
    {
        if (!g_d3dDevice)
            return false;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        g_d3dDevice->GetImmediateContext(&context);
        if (!context)
            return false;
        void** table = *reinterpret_cast<void***>(context.Get());
        if (MH_CreateHook(table[33], HookedOmSetRenderTargets,
                reinterpret_cast<void**>(&g_omSetRenderTargets)) != MH_OK ||
            MH_CreateHook(table[44], HookedRsSetViewports,
                reinterpret_cast<void**>(&g_rsSetViewports)) != MH_OK ||
            MH_CreateHook(table[50], HookedClearRtv,
                reinterpret_cast<void**>(&g_clearRtv)) != MH_OK ||
            MH_CreateHook(table[53], HookedClearDsv,
                reinterpret_cast<void**>(&g_clearDsv)) != MH_OK ||
            MH_CreateHook(table[47], HookedCopyResource,
                reinterpret_cast<void**>(&g_copyResource)) != MH_OK ||
            MH_CreateHook(table[57], HookedResolve,
                reinterpret_cast<void**>(&g_resolve)) != MH_OK ||
            MH_CreateHook(table[12], HookedDrawIndexed,
                reinterpret_cast<void**>(&g_drawIndexed)) != MH_OK ||
            MH_CreateHook(table[13], HookedDraw,
                reinterpret_cast<void**>(&g_draw)) != MH_OK)
            return false;
        for (const std::size_t index : {33u, 44u, 50u, 53u, 47u, 57u, 12u, 13u})
            if (MH_EnableHook(table[index]) != MH_OK)
                return false;
        return true;
    }

    const char* ApiName(ApiKind kind) noexcept
    {
        switch (kind)
        {
        case ApiKind::Targets: return "OM targets";
        case ApiKind::Viewport: return "viewport";
        case ApiKind::ClearColor: return "clear color";
        case ApiKind::ClearDepth: return "clear depth";
        case ApiKind::Copy: return "copy";
        case ApiKind::Resolve: return "resolve";
        case ApiKind::DrawIndexed: return "HUD candidate DrawIndexed";
        case ApiKind::Draw: return "HUD candidate Draw";
        }
        return "api";
    }

    bool MapGuiClientPoint(POINT& point) noexcept
    {
        if (!g_gameWindow)
            return false;
        RECT client{};
        if (!GetClientRect(g_gameWindow, &client))
            return false;
        const float clientWidth = static_cast<float>(client.right - client.left);
        const float clientHeight = static_cast<float>(client.bottom - client.top);
        const float nativeWidth = static_cast<float>(g_guiNativeWidth.load());
        const float nativeHeight = static_cast<float>(g_guiNativeHeight.load());
        if (clientWidth <= 0.0f || clientHeight <= 0.0f || nativeWidth <= 0.0f ||
            nativeHeight <= 0.0f)
            return false;
        const float backWidth = static_cast<float>(g_guiBackWidth.load());
        const float backHeight = static_cast<float>(g_guiBackHeight.load());
        if (g_hudCompositeWidth > 0.0f && g_hudCompositeHeight > 0.0f &&
            backWidth > 0.0f && backHeight > 0.0f)
        {
            const float compositeWidth = (std::clamp)(g_hudCompositeWidth, 64.0f,
                backWidth);
            const float compositeHeight = (std::clamp)(g_hudCompositeHeight, 64.0f,
                backHeight);
            const float contentScale = (std::clamp)(g_hudContentScale, 0.05f, 4.0f);
            const float contentWidth = compositeWidth * contentScale;
            const float contentHeight = compositeHeight * contentScale;
            const unsigned eye = dayz::stereo_state::RenderedEye();
            const float eyeOffsetX = eye == 0 ? g_hudLeftOffsetX : g_hudRightOffsetX;
            const float left = (backWidth - contentWidth) * 0.5f + eyeOffsetX;
            const float top = (backHeight - contentHeight) * 0.5f;
            const float backX = static_cast<float>(point.x) * backWidth / clientWidth;
            const float backY = static_cast<float>(point.y) * backHeight / clientHeight;
            point.x = static_cast<LONG>(std::lround((backX - left) * nativeWidth /
                contentWidth));
            point.y = static_cast<LONG>(std::lround((backY - top) * nativeHeight /
                contentHeight));
        }
        else
        {
            point.x = static_cast<LONG>(std::lround(static_cast<float>(point.x) *
                nativeWidth / clientWidth));
            point.y = static_cast<LONG>(std::lround(static_cast<float>(point.y) *
                nativeHeight / clientHeight));
        }
        point.x = (std::clamp)(point.x, 0L, static_cast<LONG>(nativeWidth) - 1);
        point.y = (std::clamp)(point.y, 0L, static_cast<LONG>(nativeHeight) - 1);
        return true;
    }

    bool RawGuiCursorModeActive() noexcept
    {
        if (!g_guiMouseRemapEnabled || !g_gameWindow ||
            GetForegroundWindow() != g_gameWindow)
            return false;
        CURSORINFO info{sizeof(info)};
        const BOOL result = g_getCursorInfo ? g_getCursorInfo(&info) : GetCursorInfo(&info);
        return result && (info.flags & CURSOR_SHOWING) != 0;
    }

    bool IsGuiCursorModeActive() noexcept
    {
        return g_guiCursorDebounced.load(std::memory_order_relaxed) &&
            RawGuiCursorModeActive();
    }

    void UpdateNativeHmdAim() noexcept
    {
        if (!g_hmdRotationEnabled || !g_hmdNativeAimEnabled)
            return;
        const dayz::stereo_state::HmdOrientation orientation =
            dayz::stereo_state::GetHmdOrientation();
        if (!orientation.valid)
            return;
        const Quaternion current = Normalize({orientation.x, orientation.y, orientation.z,
            orientation.w});
        const float yaw = std::atan2(
            2.0f * (current.w * current.y + current.x * current.z),
            1.0f - 2.0f * (current.x * current.x + current.y * current.y));
        const float pitch = std::asin((std::clamp)(
            2.0f * (current.w * current.x - current.z * current.y), -1.0f, 1.0f));
        if (!g_haveNativeHmdAngles)
        {
            g_previousHmdYaw = yaw;
            g_previousHmdPitch = pitch;
            g_haveNativeHmdAngles = true;
            return;
        }

        const float yawDelta = (std::clamp)(
            std::remainder(yaw - g_previousHmdYaw, 2.0f * 3.14159265358979323846f),
            -0.35f, 0.35f);
        const float pitchDelta = (std::clamp)(pitch - g_previousHmdPitch, -0.35f, 0.35f);
        g_previousHmdYaw = yaw;
        g_previousHmdPitch = pitch;

        if (!g_gameWindow || GetForegroundWindow() != g_gameWindow)
            return;

        g_pendingMouseX += static_cast<double>(yawDelta) * g_hmdMouseYawScale;
        g_pendingMouseY += static_cast<double>(pitchDelta) * g_hmdMousePitchScale;
        if (IsGuiCursorModeActive())
        {
            // DayZ blocks gameplay mouse-look while the inventory owns input.
            // Keep the deltas queued; the render camera follows HMD directly
            // meanwhile, and the game camera catches up when inventory closes.
            if (!g_inventoryHmdLookEnabled)
            {
                g_pendingMouseX = 0.0;
                g_pendingMouseY = 0.0;
            }
            return;
        }
        const LONG mouseX = static_cast<LONG>(std::trunc(g_pendingMouseX));
        const LONG mouseY = static_cast<LONG>(std::trunc(g_pendingMouseY));
        g_pendingMouseX -= mouseX;
        g_pendingMouseY -= mouseY;
        if (!mouseX && !mouseY)
            return;

        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = mouseX;
        input.mi.dy = mouseY;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &input, sizeof(input));
    }

    bool CenterPhysicalCursor(POINT& center) noexcept
    {
        RECT client{};
        if (!g_gameWindow || !GetClientRect(g_gameWindow, &client))
            return false;
        center.x = (client.right - client.left) / 2;
        center.y = (client.bottom - client.top) / 2;
        POINT screenCenter = center;
        return ClientToScreen(g_gameWindow, &screenCenter) &&
            SetCursorPos(screenCenter.x, screenCenter.y);
    }

    bool UpdateVirtualCursorFromMouseMove(POINT physicalPoint) noexcept
    {
        if (!IsGuiCursorModeActive())
        {
            g_guiVirtualCursorActive = false;
            return false;
        }
        if (!g_guiVirtualCursorActive.load())
        {
            const LONG nativeWidth = static_cast<LONG>(g_guiNativeWidth.load());
            const LONG nativeHeight = static_cast<LONG>(g_guiNativeHeight.load());
            if (nativeWidth <= 0 || nativeHeight <= 0)
                return false;
            g_guiVirtualCursorX = nativeWidth / 2;
            g_guiVirtualCursorY = nativeHeight / 2;
            g_guiVirtualCursorActive = true;
            POINT ignored{};
            CenterPhysicalCursor(ignored);
            return true;
        }

        RECT client{};
        if (!GetClientRect(g_gameWindow, &client))
            return false;
        const LONG clientWidth = client.right - client.left;
        const LONG clientHeight = client.bottom - client.top;
        const LONG nativeWidth = static_cast<LONG>(g_guiNativeWidth.load());
        const LONG nativeHeight = static_cast<LONG>(g_guiNativeHeight.load());
        if (clientWidth <= 0 || clientHeight <= 0 || nativeWidth <= 0 || nativeHeight <= 0)
            return false;

        const POINT center{clientWidth / 2, clientHeight / 2};
        const LONG deltaX = physicalPoint.x - center.x;
        const LONG deltaY = physicalPoint.y - center.y;
        if (deltaX || deltaY)
        {
            const LONG movedX = static_cast<LONG>(std::lround(static_cast<double>(deltaX) *
                nativeWidth / clientWidth));
            const LONG movedY = static_cast<LONG>(std::lround(static_cast<double>(deltaY) *
                nativeHeight / clientHeight));
            g_guiVirtualCursorX = (std::clamp)(g_guiVirtualCursorX.load() + movedX,
                0L, nativeWidth - 1);
            g_guiVirtualCursorY = (std::clamp)(g_guiVirtualCursorY.load() + movedY,
                0L, nativeHeight - 1);
            POINT ignored{};
            CenterPhysicalCursor(ignored);
        }
        return true;
    }

    bool GetVirtualCursorPoint(POINT& point) noexcept
    {
        if (!g_guiVirtualCursorActive.load() || !IsGuiCursorModeActive())
            return false;
        point.x = g_guiVirtualCursorX.load();
        point.y = g_guiVirtualCursorY.load();
        return true;
    }

    std::int64_t __fastcall HookedGuiInputMessage(void* inputManager, std::int64_t argument2,
        unsigned int message, std::int64_t wparam, std::uintptr_t messageData)
    {
        if (g_guiMouseRemapEnabled && message == WM_MOUSEMOVE)
        {
            POINT point{static_cast<SHORT>(LOWORD(messageData)),
                static_cast<SHORT>(HIWORD(messageData))};
            if (g_guiVirtualCursorActive.load() && GetVirtualCursorPoint(point))
                messageData = static_cast<std::uintptr_t>(MAKELPARAM(
                    static_cast<SHORT>(point.x), static_cast<SHORT>(point.y)));
        }
        return g_guiInputMessage(inputManager, argument2, message, wparam, messageData);
    }

    bool HasMouseCoordinates(UINT message) noexcept
    {
        switch (message)
        {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
            return true;
        default:
            return false;
        }
    }

    LRESULT CALLBACK HookedGameWindowProcedure(HWND window, UINT message, WPARAM wparam,
        LPARAM lparam)
    {
        if (g_guiMouseRemapEnabled && HasMouseCoordinates(message))
        {
            POINT point{static_cast<SHORT>(LOWORD(lparam)),
                static_cast<SHORT>(HIWORD(lparam))};
            if (message == WM_MOUSEMOVE)
                UpdateVirtualCursorFromMouseMove(point);
            if (GetVirtualCursorPoint(point) || MapGuiClientPoint(point))
                lparam = MAKELPARAM(static_cast<SHORT>(point.x), static_cast<SHORT>(point.y));
        }
        return CallWindowProcW(g_originalWindowProcedure, window, message, wparam, lparam);
    }

    BOOL WINAPI HookedGetCursorPos(LPPOINT point)
    {
        const BOOL result = g_getCursorPos(point);
        if (!result || !point || !g_guiMouseRemapEnabled || !g_gameWindow)
            return result;
        POINT virtualPoint{};
        if (GetVirtualCursorPoint(virtualPoint) && ClientToScreen(g_gameWindow, &virtualPoint))
        {
            if (!g_guiVirtualCursorApiLogged.exchange(true))
            {
                const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
                std::ostringstream message;
                message << "GUI virtual cursor supplied through GetCursorPos caller=0x"
                    << std::hex << caller;
                if (caller >= g_moduleBase && caller < g_moduleBase + kImageSize)
                    message << " DayZ+rva=0x" << (caller - g_moduleBase);
                logging::Info(message.str());
            }
            *point = virtualPoint;
            return result;
        }
        POINT clientPoint = *point;
        if (!ScreenToClient(g_gameWindow, &clientPoint) || !MapGuiClientPoint(clientPoint) ||
            !ClientToScreen(g_gameWindow, &clientPoint))
            return result;
        *point = clientPoint;
        return result;
    }

    BOOL WINAPI HookedGetCursorInfo(PCURSORINFO info)
    {
        const BOOL result = g_getCursorInfo(info);
        if (!result || !info || !g_guiMouseRemapEnabled || !g_gameWindow ||
            !g_guiVirtualCursorActive.load() || GetForegroundWindow() != g_gameWindow ||
            !(info->flags & CURSOR_SHOWING))
            return result;
        POINT virtualPoint{g_guiVirtualCursorX.load(), g_guiVirtualCursorY.load()};
        if (ClientToScreen(g_gameWindow, &virtualPoint))
            info->ptScreenPos = virtualPoint;
        return result;
    }

    void InstallGuiMouseApiHook() noexcept
    {
        if (!g_guiMouseRemapEnabled || g_getCursorPos || g_getCursorInfo)
            return;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        void* cursorPosTarget = user32 ? reinterpret_cast<void*>(GetProcAddress(user32,
            "GetCursorPos")) : nullptr;
        void* cursorInfoTarget = user32 ? reinterpret_cast<void*>(GetProcAddress(user32,
            "GetCursorInfo")) : nullptr;
        if (!cursorPosTarget || !cursorInfoTarget ||
            MH_CreateHook(cursorPosTarget, HookedGetCursorPos,
                reinterpret_cast<void**>(&g_getCursorPos)) != MH_OK ||
            MH_CreateHook(cursorInfoTarget, HookedGetCursorInfo,
                reinterpret_cast<void**>(&g_getCursorInfo)) != MH_OK ||
            MH_EnableHook(cursorPosTarget) != MH_OK ||
            MH_EnableHook(cursorInfoTarget) != MH_OK)
        {
            logging::Error("GUI cursor API remap hooks unavailable");
            if (cursorPosTarget)
                MH_DisableHook(cursorPosTarget);
            if (cursorInfoTarget)
                MH_DisableHook(cursorInfoTarget);
            g_getCursorPos = nullptr;
            g_getCursorInfo = nullptr;
            return;
        }
        logging::Info("GUI GetCursorPos/GetCursorInfo remap hooks active");
    }

    void AttachGuiWindow(IDXGISwapChain* swapChain) noexcept
    {
        if (!swapChain)
            return;
        DXGI_SWAP_CHAIN_DESC swapDescription{};
        if (FAILED(swapChain->GetDesc(&swapDescription)) || !swapDescription.OutputWindow)
            return;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
            return;
        D3D11_TEXTURE2D_DESC description{};
        backBuffer->GetDesc(&description);
        if (description.Width < 1000 || description.Height < 1000)
            return;
        g_guiBackWidth = description.Width;
        g_guiBackHeight = description.Height;
        if (!g_gameWindow)
        {
            g_gameWindow = swapDescription.OutputWindow;
            SetLastError(0);
            const LONG_PTR previous = SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(HookedGameWindowProcedure));
            if (!previous && GetLastError() != 0)
            {
                logging::Error("GUI window mouse-message remap unavailable");
                g_gameWindow = nullptr;
                return;
            }
            g_originalWindowProcedure = reinterpret_cast<WNDPROC>(previous);
            logging::Info("GUI window mouse-message remap active");
        }
    }

    HudCompositeConstants CurrentHudCompositeConstants(float contentScale = -1.0f) noexcept
    {
        HudCompositeConstants constants{};
        constants.contentScale = contentScale > 0.0f ? contentScale :
            (std::clamp)(g_hudContentScale, 0.05f, 4.0f);
        const float nativeWidth = static_cast<float>(g_guiNativeWidth.load());
        const float nativeHeight = static_cast<float>(g_guiNativeHeight.load());
        constants.cursorPixelUv[0] = nativeWidth > 0.0f ? 1.0f / nativeWidth : 1.0f;
        constants.cursorPixelUv[1] = nativeHeight > 0.0f ? 1.0f / nativeHeight : 1.0f;
        if (!g_guiCursorEnabled || !g_gameWindow || nativeWidth <= 0.0f ||
            nativeHeight <= 0.0f || GetForegroundWindow() != g_gameWindow)
            return constants;
        CURSORINFO info{sizeof(info)};
        POINT point{};
        if (GetVirtualCursorPoint(point))
        {
            constants.cursorUv[0] = static_cast<float>(point.x) / nativeWidth;
            constants.cursorUv[1] = static_cast<float>(point.y) / nativeHeight;
            constants.cursorVisible = 1.0f;
            return constants;
        }
        const BOOL positioned = g_getCursorPos ? g_getCursorPos(&point) : GetCursorPos(&point);
        const BOOL cursorInfoAvailable = g_getCursorInfo ? g_getCursorInfo(&info) :
            GetCursorInfo(&info);
        if (!positioned || !cursorInfoAvailable || !(info.flags & CURSOR_SHOWING) ||
            !ScreenToClient(g_gameWindow, &point))
            return constants;
        RECT client{};
        if (!GetClientRect(g_gameWindow, &client) || point.x < 0 || point.y < 0 ||
            point.x >= client.right || point.y >= client.bottom || !MapGuiClientPoint(point))
            return constants;
        constants.cursorUv[0] = static_cast<float>(point.x) / nativeWidth;
        constants.cursorUv[1] = static_cast<float>(point.y) / nativeHeight;
        constants.cursorVisible = 1.0f;
        return constants;
    }

    bool EnsureHudCompositePipeline() noexcept
    {
        if (g_hudCompositeContext && g_hudCompositeVs && g_hudCompositePs &&
            g_hudCompositeSampler && g_hudCompositeBlend && g_hudCompositeDepth &&
            g_hudCompositeRasterizer && g_hudCompositeConstants)
            return true;
        if (!g_d3dDevice || FAILED(g_d3dDevice->CreateDeferredContext(0,
                &g_hudCompositeContext)))
            return false;
        Microsoft::WRL::ComPtr<ID3DBlob> vertexCode;
        Microsoft::WRL::ComPtr<ID3DBlob> pixelCode;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        if (FAILED(D3DCompile(kHudCompositeShader, sizeof(kHudCompositeShader) - 1,
                "dayz_hud_layer.hlsl", nullptr, nullptr, "VSMain", "vs_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                &vertexCode, &errors)) ||
            FAILED(D3DCompile(kHudCompositeShader, sizeof(kHudCompositeShader) - 1,
                "dayz_hud_layer.hlsl", nullptr, nullptr, "PSMain", "ps_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                &pixelCode, &errors)))
            return false;
        if (FAILED(g_d3dDevice->CreateVertexShader(vertexCode->GetBufferPointer(),
                vertexCode->GetBufferSize(), nullptr, &g_hudCompositeVs)) ||
            FAILED(g_d3dDevice->CreatePixelShader(pixelCode->GetBufferPointer(),
                pixelCode->GetBufferSize(), nullptr, &g_hudCompositePs)))
            return false;
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BLEND_DESC blend{};
        auto& target = blend.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_ONE;
        target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = sizeof(HudCompositeConstants);
        constants.Usage = D3D11_USAGE_DEFAULT;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return SUCCEEDED(g_d3dDevice->CreateSamplerState(&sampler, &g_hudCompositeSampler)) &&
            SUCCEEDED(g_d3dDevice->CreateBlendState(&blend, &g_hudCompositeBlend)) &&
            SUCCEEDED(g_d3dDevice->CreateDepthStencilState(&depth, &g_hudCompositeDepth)) &&
            SUCCEEDED(g_d3dDevice->CreateRasterizerState(&rasterizer,
                &g_hudCompositeRasterizer)) &&
            SUCCEEDED(g_d3dDevice->CreateBuffer(&constants, nullptr,
                &g_hudCompositeConstants));
    }

    void CompositeHudLayer(IDXGISwapChain* swapChain) noexcept
    {
        if (!swapChain || !g_hudLayerDirty.exchange(false))
            return;
        std::scoped_lock lock(g_hudLayerMutex);
        if (!g_hudLayerTexture || !g_hudLayerResolved || !g_hudLayerView ||
            !EnsureHudCompositePipeline())
            return;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
            return;
        D3D11_TEXTURE2D_DESC backDescription{};
        backBuffer->GetDesc(&backDescription);
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> backTarget;
        if (FAILED(g_d3dDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, &backTarget)))
            return;

        g_hudCompositeContext->ClearState();
        if (g_hudLayerSamples.Count > 1)
            g_hudCompositeContext->ResolveSubresource(g_hudLayerResolved.Get(), 0,
                g_hudLayerTexture.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        else
            g_hudCompositeContext->CopyResource(g_hudLayerResolved.Get(),
                g_hudLayerTexture.Get());
        ID3D11RenderTargetView* target = backTarget.Get();
        g_hudCompositeContext->OMSetRenderTargets(1, &target, nullptr);
        const bool explicitComposite =
            g_hudCompositeWidth > 0.0f && g_hudCompositeHeight > 0.0f;
        const float automaticScale =
            (std::clamp)(g_overrideHudScale ? g_hudScale : 1.0f, 0.05f, 1.0f);
        const float requestedWidth = explicitComposite ? g_hudCompositeWidth :
            static_cast<float>(backDescription.Width) * automaticScale;
        const float requestedHeight = explicitComposite ? g_hudCompositeHeight :
            static_cast<float>(backDescription.Height) * automaticScale;
        const float width = (std::clamp)(requestedWidth, 64.0f,
            static_cast<float>(backDescription.Width));
        const float height = (std::clamp)(requestedHeight, 64.0f,
            static_cast<float>(backDescription.Height));
        const unsigned hudEye = dayz::stereo_state::RenderedEye();
        const float eyeOffsetX = hudEye == 0 ? g_hudLeftOffsetX : g_hudRightOffsetX;
        const D3D11_VIEWPORT viewport{
            (backDescription.Width - width) * 0.5f + eyeOffsetX,
            (backDescription.Height - height) * 0.5f,
            width, height, 0.0f, 1.0f};
        g_hudCompositeContext->RSSetViewports(1, &viewport);
        g_hudCompositeContext->RSSetState(g_hudCompositeRasterizer.Get());
        g_hudCompositeContext->OMSetDepthStencilState(g_hudCompositeDepth.Get(), 0);
        constexpr FLOAT blendFactor[4]{};
        g_hudCompositeContext->OMSetBlendState(g_hudCompositeBlend.Get(), blendFactor,
            0xFFFFFFFFu);
        g_hudCompositeContext->IASetInputLayout(nullptr);
        g_hudCompositeContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_hudCompositeContext->VSSetShader(g_hudCompositeVs.Get(), nullptr, 0);
        g_hudCompositeContext->PSSetShader(g_hudCompositePs.Get(), nullptr, 0);
        const HudCompositeConstants constants = CurrentHudCompositeConstants();
        g_hudCompositeContext->UpdateSubresource(g_hudCompositeConstants.Get(), 0, nullptr,
            &constants, 0, 0);
        ID3D11ShaderResourceView* view = g_hudLayerView.Get();
        ID3D11SamplerState* sampler = g_hudCompositeSampler.Get();
        ID3D11Buffer* constantBuffer = g_hudCompositeConstants.Get();
        g_hudCompositeContext->PSSetShaderResources(0, 1, &view);
        g_hudCompositeContext->PSSetSamplers(0, 1, &sampler);
        g_hudCompositeContext->PSSetConstantBuffers(0, 1, &constantBuffer);
        g_hudCompositeContext->Draw(3, 0);
        view = nullptr;
        g_hudCompositeContext->PSSetShaderResources(0, 1, &view);
        Microsoft::WRL::ComPtr<ID3D11CommandList> commands;
        if (SUCCEEDED(g_hudCompositeContext->FinishCommandList(FALSE, &commands)))
        {
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
            g_d3dDevice->GetImmediateContext(&immediate);
            if (immediate)
            {
                immediate->ExecuteCommandList(commands.Get(), TRUE);
                constexpr FLOAT transparent[4]{};
                immediate->ClearRenderTargetView(g_hudLayerTarget.Get(), transparent);
                g_hudLayerNeedsClear = false;
            }
        }
    }

    bool RenderGuiQuadLayer(ID3D11RenderTargetView* target, std::uint32_t width,
        std::uint32_t height) noexcept
    {
        if (!target || !width || !height)
            return false;
        std::scoped_lock lock(g_guiLayerMutex);
        if (!g_guiLayerTexture || !g_guiLayerResolved || !g_guiLayerView ||
            !EnsureHudCompositePipeline())
            return false;

        Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
        g_d3dDevice->GetImmediateContext(&immediate);
        if (!immediate)
            return false;
        if (g_guiLayerDirty.exchange(false))
        {
            if (g_guiLayerSamples.Count > 1)
                immediate->ResolveSubresource(g_guiLayerResolved.Get(), 0,
                    g_guiLayerTexture.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
            else
                immediate->CopyResource(g_guiLayerResolved.Get(), g_guiLayerTexture.Get());
            constexpr FLOAT transparent[4]{};
            immediate->ClearRenderTargetView(g_guiLayerTarget.Get(), transparent);
            g_guiLayerNeedsClear = false;
        }

        g_hudCompositeContext->ClearState();
        constexpr FLOAT transparent[4]{};
        g_hudCompositeContext->ClearRenderTargetView(target, transparent);
        g_hudCompositeContext->OMSetRenderTargets(1, &target, nullptr);
        const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width),
            static_cast<float>(height), 0.0f, 1.0f};
        g_hudCompositeContext->RSSetViewports(1, &viewport);
        g_hudCompositeContext->RSSetState(g_hudCompositeRasterizer.Get());
        g_hudCompositeContext->OMSetDepthStencilState(g_hudCompositeDepth.Get(), 0);
        constexpr FLOAT blendFactor[4]{};
        g_hudCompositeContext->OMSetBlendState(g_hudCompositeBlend.Get(), blendFactor,
            0xFFFFFFFFu);
        g_hudCompositeContext->IASetInputLayout(nullptr);
        g_hudCompositeContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_hudCompositeContext->VSSetShader(g_hudCompositeVs.Get(), nullptr, 0);
        g_hudCompositeContext->PSSetShader(g_hudCompositePs.Get(), nullptr, 0);
        const HudCompositeConstants constants = CurrentHudCompositeConstants(1.0f);
        g_hudCompositeContext->UpdateSubresource(g_hudCompositeConstants.Get(), 0, nullptr,
            &constants, 0, 0);
        ID3D11ShaderResourceView* view = g_guiLayerView.Get();
        ID3D11SamplerState* sampler = g_hudCompositeSampler.Get();
        ID3D11Buffer* constantBuffer = g_hudCompositeConstants.Get();
        g_hudCompositeContext->PSSetShaderResources(0, 1, &view);
        g_hudCompositeContext->PSSetSamplers(0, 1, &sampler);
        g_hudCompositeContext->PSSetConstantBuffers(0, 1, &constantBuffer);
        g_hudCompositeContext->Draw(3, 0);
        view = nullptr;
        g_hudCompositeContext->PSSetShaderResources(0, 1, &view);
        Microsoft::WRL::ComPtr<ID3D11CommandList> commands;
        if (FAILED(g_hudCompositeContext->FinishCommandList(FALSE, &commands)) || !commands)
            return false;
        immediate->ExecuteCommandList(commands.Get(), TRUE);
        return true;
    }
}

namespace dayz::runtime_probe
{
    bool Initialize() noexcept
    {
        if (g_attempted.exchange(true))
            return g_active.load();
        if (!RuntimeProbeEnabled())
        {
            logging::Info("DayZ stereo runtime probe is disabled");
            return false;
        }
        if (!ValidateBuild())
        {
            logging::Error("DayZ stereo runtime probe rejected this executable: PE/signature mismatch");
            return false;
        }
        g_alternateEyeEnabled = ReadBoolean(L"stereo", L"alternate_eye", false);
        g_hmdRotationEnabled = ReadBoolean(L"stereo", L"hmd_rotation", true);
        g_hmdNativeAimEnabled = ReadBoolean(L"stereo", L"hmd_native_aim", true);
        g_hmdMouseYawScale = ReadFloat(L"stereo", L"hmd_mouse_yaw_scale", -600.0f);
        g_hmdMousePitchScale = ReadFloat(L"stereo", L"hmd_mouse_pitch_scale", -600.0f);
        g_cameraSeparation = ReadFloat(L"stereo", L"camera_separation", 0.064f);
        g_hmdPositionScale = ReadFloat(L"stereo", L"hmd_position_scale", 1.0f);
        g_gameFov = (std::clamp)(ReadFloat(L"stereo", L"game_fov", 0.0f), 0.0f, 2.8f);
        ApplyProfileFovOverride();
        ApplyActiveCameraFovOverride();
        g_hudScale = ReadFloat(L"stereo", L"hud_scale", 0.85f);
        g_overrideHudScale = ReadBoolean(L"stereo", L"override_hud_scale", false);
        g_hudSafeWidth = ReadFloat(L"stereo", L"hud_safe_width", 0.0f);
        g_hudSafeHeight = ReadFloat(L"stereo", L"hud_safe_height", 0.0f);
        g_hudCompositeWidth = ReadFloat(L"stereo", L"hud_composite_width", 0.0f);
        g_hudCompositeHeight = ReadFloat(L"stereo", L"hud_composite_height", 0.0f);
        g_hudContentScale = ReadFloat(L"stereo", L"hud_content_scale", 1.0f);
        g_hudLeftOffsetX = ReadFloat(L"stereo", L"hud_left_offset_x", 0.0f);
        g_hudRightOffsetX = ReadFloat(L"stereo", L"hud_right_offset_x", 0.0f);
        g_guiMouseRemapEnabled = ReadBoolean(L"stereo", L"gui_mouse_remap", true);
        g_guiCursorEnabled = ReadBoolean(L"stereo", L"gui_cursor", true);
        g_guiQuadEnabled = ReadBoolean(L"gui", L"quad_enabled", true);
        g_inventoryHmdLookEnabled = ReadBoolean(L"gui", L"inventory_hmd_look", true);
        g_inventoryBlurEnabled = ReadBoolean(L"gui", L"inventory_blur_enabled", false);
        g_inventoryPlayerPreviewVisible = ReadBoolean(L"gui",
            L"inventory_player_preview_visible", true);
        g_inventoryPreviewRotationScale = ReadFloat(L"gui",
            L"inventory_preview_rotation_scale", 0.5f);
        const float imageShift = ReadFloat(L"stereo", L"image_shift", 0.0f);
        dayz::stereo_state::SetImageShift(imageShift);
        const std::wstring fitModeText = ReadString(L"stereo", L"fit_mode", L"contain");
        dayz::stereo_state::FitMode fitMode = dayz::stereo_state::FitMode::Contain;
        if (_wcsicmp(fitModeText.c_str(), L"stretch") == 0)
            fitMode = dayz::stereo_state::FitMode::Stretch;
        else if (_wcsicmp(fitModeText.c_str(), L"cover") == 0)
            fitMode = dayz::stereo_state::FitMode::Cover;
        const float scaleX = ReadFloat(L"stereo", L"scale_x", 1.0f);
        const float scaleY = ReadFloat(L"stereo", L"scale_y", 1.0f);
        dayz::stereo_state::SetPresentation(fitMode, scaleX, scaleY);
        const MH_STATUS initialized = MH_Initialize();
        if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
        {
            logging::Error("DayZ stereo runtime probe could not initialize MinHook");
            return false;
        }
        InstallGuiMouseApiHook();
        bool frameRefreshHookCreated{};
        if (g_hmdRotationEnabled)
        {
            g_frameRefreshTarget = ResolveFrameRefreshTarget();
            if (g_frameRefreshTarget)
                frameRefreshHookCreated = MH_CreateHook(
                    reinterpret_cast<void*>(g_frameRefreshTarget), HookedFrameRefresh,
                    reinterpret_cast<void**>(&g_frameRefresh)) == MH_OK;
            if (!frameRefreshHookCreated)
                logging::Error("Full FrameBase refresh hook unavailable; using camera-basis fallback");
        }
        if (!AddHook(kPrepareViewRva, HookedPrepareView, g_prepareView) ||
            !AddHook(kExecuteViewRva, HookedExecuteView, g_executeView) ||
            !AddHook(kFinalizeViewRva, HookedFinalizeView, g_finalizeView) ||
            !AddHook(kProjectionDispatchRva, HookedProjectionDispatch, g_projectionDispatch) ||
            !AddHook(kHudLayoutRva, HookedHudLayout, g_hudLayout) ||
            !AddHook(kGuiInputMessageRva, HookedGuiInputMessage, g_guiInputMessage))
        {
            logging::Error("DayZ stereo runtime probe hook creation failed; hooks remain disabled");
            return false;
        }
        bool dynamicBlurHookCreated{};
        if (!g_inventoryBlurEnabled && kDynamicBlurRva && kDynamicBlurParameterIndexRva)
        {
            dynamicBlurHookCreated = AddHook(kDynamicBlurRva, HookedDynamicBlur,
                g_dynamicBlur);
            if (!dynamicBlurHookCreated)
                logging::Error("Inventory blur hook creation failed; blur remains enabled");
        }
        bool cameraFovHookCreated{};
        if (g_gameFov > 0.0f && kCameraFovUpdateRva)
        {
            cameraFovHookCreated = AddHook(kCameraFovUpdateRva, HookedCameraFovUpdate,
                g_cameraFovUpdate);
            if (!cameraFovHookCreated)
                logging::Error("Gameplay camera FOV hook creation failed");
        }
        const std::array<void*, 6> targets{
            reinterpret_cast<void*>(g_moduleBase + kPrepareViewRva),
            reinterpret_cast<void*>(g_moduleBase + kExecuteViewRva),
            reinterpret_cast<void*>(g_moduleBase + kFinalizeViewRva),
            reinterpret_cast<void*>(g_moduleBase + kProjectionDispatchRva),
            reinterpret_cast<void*>(g_moduleBase + kHudLayoutRva),
            reinterpret_cast<void*>(g_moduleBase + kGuiInputMessageRva)};
        for (void* target : targets)
            if (MH_EnableHook(target) != MH_OK)
            {
                for (void* enabledTarget : targets)
                    MH_DisableHook(enabledTarget);
                logging::Error("DayZ stereo runtime probe hook enable failed; hooks disabled");
                return false;
            }
        if (dynamicBlurHookCreated)
        {
            const MH_STATUS enabled = MH_EnableHook(
                reinterpret_cast<void*>(g_moduleBase + kDynamicBlurRva));
            if (enabled == MH_OK || enabled == MH_ERROR_ENABLED)
                logging::Info("Inventory Gauss blur disabled by [gui] setting");
            else
                logging::Error("Inventory blur hook could not be enabled; blur remains enabled");
        }
        if (cameraFovHookCreated)
        {
            const MH_STATUS enabled = MH_EnableHook(
                reinterpret_cast<void*>(g_moduleBase + kCameraFovUpdateRva));
            if (enabled == MH_OK || enabled == MH_ERROR_ENABLED)
                logging::Info("Gameplay camera FOV override hook active");
            else
                logging::Error("Gameplay camera FOV override hook could not be enabled");
        }
        if (frameRefreshHookCreated)
        {
            const MH_STATUS enabled = MH_EnableHook(
                reinterpret_cast<void*>(g_frameRefreshTarget));
            if (enabled == MH_OK || enabled == MH_ERROR_ENABLED)
            {
                g_frameRefreshHookActive = true;
                std::ostringstream message;
                message << "HMD full FrameBase refresh hook active at DayZ+0x" << std::hex
                    << (g_frameRefreshTarget - g_moduleBase);
                logging::Info(message.str());
            }
            else
                logging::Error("Full FrameBase refresh hook could not be enabled; using camera-basis fallback");
        }
        g_active = true;
        {
            std::ostringstream message;
            message << "GUI internal mouse-position remap active at DayZ+0x" << std::hex
                    << kGuiInputMessageRva;
            logging::Info(message.str());
        }
        if (InstallD3DHooks())
            logging::Info("DayZ stereo D3D11 target probe active");
        else
            logging::Error("DayZ stereo D3D11 target probe unavailable; render trace remains active");
        {
            std::ostringstream message;
            message << "DayZ stereo runtime probe active: " << g_buildProfile->name
                    << " build and five render signatures verified";
            logging::Info(message.str());
        }
        if (g_alternateEyeEnabled)
        {
            const char* fitModeName = fitMode == dayz::stereo_state::FitMode::Stretch
                ? "stretch" : fitMode == dayz::stereo_state::FitMode::Cover ? "cover" : "contain";
            std::ostringstream message;
            message << "Alternating-eye stereo validation enabled; camera_separation="
                << g_cameraSeparation << " image_shift=" << imageShift
                << " hmd_rotation=" << g_hmdRotationEnabled
                << " hmd_native_aim=" << g_hmdNativeAimEnabled
                << " game_fov=" << g_gameFov
                << " hmd_mouse_scale=" << g_hmdMouseYawScale << ','
                << g_hmdMousePitchScale
                << " fit_mode=" << fitModeName
                << " scale=" << scaleX << 'x' << scaleY
                << " hud_scale_override=" << g_overrideHudScale
                << " hud_safe=" << g_hudSafeWidth << 'x' << g_hudSafeHeight
                << " hud_composite_layer=" << g_hudCompositeWidth << 'x'
                << g_hudCompositeHeight << " hud_eye_offsets=" << g_hudLeftOffsetX << ','
                << g_hudRightOffsetX << " hud_content_scale=" << g_hudContentScale
                << " gui_mouse_remap=" << g_guiMouseRemapEnabled
                << " gui_cursor=" << g_guiCursorEnabled
                << " inventory_hmd_look=" << g_inventoryHmdLookEnabled
                << " inventory_blur=" << g_inventoryBlurEnabled
                << " inventory_preview_rotation_scale=" <<
                    g_inventoryPreviewRotationScale
                << " gui_quad=" << g_guiQuadEnabled;
            logging::Info(message.str());
        }
        return true;
    }

    void AttachD3DDevice(ID3D11Device* device) noexcept
    {
        if (device && !g_d3dDevice)
            g_d3dDevice = device;
    }

    void BeforePresent(IDXGISwapChain* swapChain) noexcept
    {
        if (g_active.load(std::memory_order_relaxed))
        {
            AttachGuiWindow(swapChain);
            CompositeHudLayer(swapChain);
        }
    }

    void OnPresent() noexcept
    {
        if (!g_active.load(std::memory_order_relaxed))
            return;
        if (RawGuiCursorModeActive())
        {
            const unsigned visible = g_guiCursorVisibleFrames.fetch_add(1,
                std::memory_order_relaxed) + 1;
            g_guiCursorDebounced.store(visible >= 2, std::memory_order_relaxed);
        }
        else
        {
            g_guiCursorVisibleFrames.store(0, std::memory_order_relaxed);
            g_guiCursorDebounced.store(false, std::memory_order_relaxed);
        }
        ApplyProfileFovOverride();
        ApplyActiveCameraFovOverride();
        UpdateNativeHmdAim();
        if (!IsGuiCursorModeActive())
            ResetInventoryPreviewAnchor();
        else
            g_inventoryPreviewOrdinal = 0;
        const std::uint64_t frame = g_presentCount.fetch_add(1) + 1;
        if (g_overrideHudScale)
            WriteHudScale();
        ApplyHudSafeAreaFromEngineSingleton();
        const std::uint32_t count = g_eventCount.exchange(0, std::memory_order_acq_rel);
        const std::uint32_t apiCount = g_apiEventCount.exchange(0, std::memory_order_acq_rel);
        const std::uint32_t drawStateCount = g_drawStateEventCount.exchange(0,
            std::memory_order_acq_rel);
        g_captureApi.store(frame < 12 || frame % 120 == 119, std::memory_order_relaxed);
        if (g_alternateEyeEnabled)
            dayz::stereo_state::AdvanceEye();
        if (frame > 12 && frame % 120 != 0)
            return;

        std::ostringstream summary;
        summary << "DayZ render trace frame=" << frame << " events=" << count;
        logging::Info(summary.str());
        const std::size_t visible = (std::min)(static_cast<std::size_t>(count), g_events.size());
        for (std::size_t index = 0; index < visible; ++index)
        {
            const Event& event = g_events[index];
            std::ostringstream line;
            line << "  #" << index << ' ' << EventName(event.kind)
                 << " tid=" << event.thread << " mode=" << static_cast<unsigned>(event.mode)
                 << " ctx=" << event.context << " cam=" << event.camera
                 << " desc=" << event.descriptor << " arena=0x" << std::hex
                 << event.arenaCursor << " caller=DayZ+0x" << event.callerRva;
            logging::Info(line.str());
        }
        if (count > g_events.size())
            logging::Info("  trace truncated; increase runtime probe event capacity if needed");
        const std::size_t apiVisible = (std::min)(static_cast<std::size_t>(apiCount),
            g_apiEvents.size());
        for (std::size_t index = 0; index < apiVisible; ++index)
        {
            const ApiEvent& event = g_apiEvents[index];
            std::ostringstream line;
            line << "  D3D #" << index << ' ' << ApiName(event.kind)
                 << " tid=" << event.thread << " a=" << event.object0 << " b=" << event.object1
                 << " size=" << event.width << 'x' << event.height << " fmt=" << event.format
                 << " samples=" << event.samples;
            if (event.kind == ApiKind::Draw || event.kind == ApiKind::DrawIndexed)
                line << " target=" << event.targetWidth << 'x' << event.targetHeight
                     << " count=" << event.elementCount << " caller=DayZ+0x" << std::hex
                     << event.callerRva;
            logging::Info(line.str());
        }
        if (apiCount > g_apiEvents.size())
            logging::Info("  D3D trace truncated");
        const std::size_t drawStateVisible = (std::min)(
            static_cast<std::size_t>(drawStateCount), g_drawStateEvents.size());
        for (std::size_t index = 0; index < drawStateVisible; ++index)
        {
            const DrawStateEvent& event = g_drawStateEvents[index];
            std::ostringstream line;
            line << "  ALPHA #" << index << ' '
                 << (event.kind == ApiKind::Draw ? "Draw" : "DrawIndexed")
                 << " tid=" << event.thread << " PS=" << event.pixelShader
                 << " VS=" << event.vertexShader << " blend=" << event.blendState
                 << " depth=" << event.depthState << " target=" << event.targetWidth << 'x'
                 << event.targetHeight << " fmt=" << event.targetFormat
                 << " viewport=" << event.viewportX << ',' << event.viewportY << '+'
                 << event.viewportWidth << 'x' << event.viewportHeight
                 << " depthEnabled=" << event.depthEnabled << " depthWrite=" << event.depthWrite
                 << " count=" << event.elementCount << " caller=DayZ+0x" << std::hex
                 << event.callerRva << " parent=DayZ+0x" << event.parentCallerRva;
            logging::Info(line.str());
        }
        if (drawStateCount > g_drawStateEvents.size())
            logging::Info("  alpha draw-state trace truncated");
    }

    bool IsGuiQuadVisible() noexcept
    {
        return g_active.load(std::memory_order_relaxed) && g_guiQuadEnabled &&
            IsGuiCursorModeActive() && g_guiLayerView &&
            g_guiLayerCapturedPresent.load(std::memory_order_relaxed) ==
                g_presentCount.load(std::memory_order_relaxed);
    }

    void SetGuiVirtualCursorNormalized(float u, float v) noexcept
    {
        const auto width = g_guiNativeWidth.load(std::memory_order_relaxed);
        const auto height = g_guiNativeHeight.load(std::memory_order_relaxed);
        if (!width || !height)
            return;
        u = (std::clamp)(u, 0.0f, 1.0f);
        v = (std::clamp)(v, 0.0f, 1.0f);
        g_guiVirtualCursorX.store(static_cast<LONG>(std::lround(u * (width - 1))),
            std::memory_order_relaxed);
        g_guiVirtualCursorY.store(static_cast<LONG>(std::lround(v * (height - 1))),
            std::memory_order_relaxed);
        g_guiVirtualCursorActive.store(true, std::memory_order_release);
    }

    bool RenderGuiQuad(ID3D11RenderTargetView* target, std::uint32_t width,
        std::uint32_t height) noexcept
    {
        return IsGuiQuadVisible() && RenderGuiQuadLayer(target, width, height);
    }

    bool IsActive() noexcept
    {
        return g_active.load(std::memory_order_relaxed);
    }
}
