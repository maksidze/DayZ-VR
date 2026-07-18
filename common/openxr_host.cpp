#include "openxr_host.hpp"

#include "debug_frame_source.hpp"
#include "dayz_frame_source.hpp"
#include "stereo_state.hpp"
#include "logging.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

namespace
{
    bool SameLuid(const LUID& left, const LUID& right) noexcept
    {
        return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
    }

    std::wstring ConfigurationPath()
    {
        std::wstring executablePath(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(),
            static_cast<DWORD>(executablePath.size()));
        if (length == 0 || length >= executablePath.size())
            return L"dayz_openxr.ini";
        executablePath.resize(length);
        const auto separator = executablePath.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
            return L"dayz_openxr.ini";
        executablePath.resize(separator + 1);
        executablePath += L"dayz_openxr.ini";
        return executablePath;
    }

    bool IsOpenXrEnabled() noexcept
    {
        wchar_t value[16]{};
        const std::wstring configPath = ConfigurationPath();
        GetPrivateProfileStringW(L"openxr", L"enabled", L"true", value,
            static_cast<DWORD>(std::size(value)), configPath.c_str());
        return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
            _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
    }
}

OpenXrHost& OpenXrHost::Instance() noexcept
{
    static OpenXrHost host;
    return host;
}

bool OpenXrHost::Check(XrResult result, const char* operation) const noexcept
{
    if (XR_SUCCEEDED(result))
        return true;
    logging::XrError(operation, result);
    return false;
}

bool OpenXrHost::CreateInstanceAndSystem()
{
    logging::Info("Enumerating OpenXR instance extensions");
    std::uint32_t extensionCount{};
    if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr),
        "xrEnumerateInstanceExtensionProperties(count)"))
        return false;

    std::vector<XrExtensionProperties> extensions(extensionCount);
    for (auto& extension : extensions)
        extension.type = XR_TYPE_EXTENSION_PROPERTIES;
    if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount,
        extensions.data()), "xrEnumerateInstanceExtensionProperties(list)"))
        return false;

    const bool hasD3D11 = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension)
    {
        return std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
    });
    if (!hasD3D11)
    {
        logging::Error("OpenXR runtime does not expose XR_KHR_D3D11_enable");
        return false;
    }

    const char* enabledExtensions[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    logging::Info("Creating OpenXR instance");
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "DayZ OpenXR");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "DayZ VR Mod");
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = enabledExtensions;
    if (!Check(xrCreateInstance(&createInfo, &instance_), "xrCreateInstance"))
        return false;

    logging::Info("Requesting HMD system");
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(instance_, &systemInfo, &systemId_), "xrGetSystem"))
        return false;

    logging::Info("Resolving D3D11 graphics requirements entry point");
    PFN_xrVoidFunction function{};
    if (!Check(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR", &function),
        "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)"))
        return false;
    getD3D11Requirements_ = reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(function);
    return getD3D11Requirements_ != nullptr;
}

bool OpenXrHost::ValidateDevice(ID3D11Device* device)
{
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Check(getD3D11Requirements_(instance_, systemId_, &requirements),
        "xrGetD3D11GraphicsRequirementsKHR"))
        return false;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&description)))
    {
        logging::Error("Cannot resolve the DXGI adapter for the D3D11 device");
        return false;
    }
    if (!SameLuid(description.AdapterLuid, requirements.adapterLuid))
    {
        logging::Error("D3D11 adapter LUID does not match the OpenXR runtime requirement");
        return false;
    }
    return true;
}

bool OpenXrHost::CreateCompatibleDevice()
{
    logging::Info("Reading OpenXR D3D11 graphics requirements");
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Check(getD3D11Requirements_(instance_, systemId_, &requirements),
        "xrGetD3D11GraphicsRequirementsKHR"))
        return false;

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    logging::Info("Selecting the OpenXR-required DXGI adapter");
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> selected;
    for (UINT index = 0; ; ++index)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) &&
            SameLuid(description.AdapterLuid, requirements.adapterLuid))
        {
            selected = adapter;
            break;
        }
    }
    if (!selected)
    {
        logging::Error("OpenXR requested adapter was not found");
        return false;
    }

    const D3D_FEATURE_LEVEL requested[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1
    };
    D3D_FEATURE_LEVEL created{};
    logging::Info("Creating standalone D3D11 device");
    const HRESULT result = D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested, static_cast<UINT>(std::size(requested)),
        D3D11_SDK_VERSION, &device_, &created, &context_);
    if (FAILED(result) || created < requirements.minFeatureLevel)
    {
        logging::Error("Could not create a compatible D3D11 device");
        return false;
    }
    return true;
}

bool OpenXrHost::CreateSession()
{
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device_.Get();
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = systemId_;
    return Check(xrCreateSession(instance_, &sessionInfo, &session_), "xrCreateSession");
}

bool OpenXrHost::CreateSpaces()
{
    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    info.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(xrCreateReferenceSpace(session_, &info, &localSpace_), "xrCreateReferenceSpace(local)"))
        return false;
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    return Check(xrCreateReferenceSpace(session_, &info, &viewSpace_), "xrCreateReferenceSpace(view)");
}

bool OpenXrHost::CreateSwapchains()
{
    logging::Info("Enumerating stereo view configuration");
    std::uint32_t viewCount{};
    if (!Check(xrEnumerateViewConfigurationViews(instance_, systemId_,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr),
        "xrEnumerateViewConfigurationViews(count)") || viewCount < 2)
        return false;

    std::vector<XrViewConfigurationView> configs(viewCount);
    for (auto& config : configs)
        config.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    if (!Check(xrEnumerateViewConfigurationViews(instance_, systemId_,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, configs.data()),
        "xrEnumerateViewConfigurationViews(list)"))
        return false;

    std::uint32_t formatCount{};
    logging::Info("Enumerating OpenXR swapchain formats");
    if (!Check(xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr),
        "xrEnumerateSwapchainFormats(count)"))
        return false;
    std::vector<std::int64_t> formats(formatCount);
    if (!Check(xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data()),
        "xrEnumerateSwapchainFormats(list)"))
        return false;
    const DXGI_FORMAT preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM};
    DXGI_FORMAT selected = DXGI_FORMAT_UNKNOWN;
    for (const auto candidate : preferred)
        if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(candidate)) != formats.end())
        {
            selected = candidate;
            break;
        }
    if (selected == DXGI_FORMAT_UNKNOWN)
    {
        logging::Error("OpenXR runtime has no supported RGBA swapchain format");
        return false;
    }

    for (std::size_t eye = 0; eye < eyeSwapchains_.size(); ++eye)
    {
        auto& swapchain = eyeSwapchains_[eye];
        logging::Info(eye == 0 ? "Creating left-eye swapchain" : "Creating right-eye swapchain");
        swapchain.width = configs[eye].recommendedImageRectWidth;
        swapchain.height = configs[eye].recommendedImageRectHeight;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = selected;
        info.sampleCount = 1;
        info.width = swapchain.width;
        info.height = swapchain.height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!Check(xrCreateSwapchain(session_, &info, &swapchain.handle), "xrCreateSwapchain"))
            return false;

        logging::Info(eye == 0 ? "Enumerating left-eye images" : "Enumerating right-eye images");
        std::uint32_t imageCount{};
        if (!Check(xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr),
            "xrEnumerateSwapchainImages(count)"))
            return false;
        swapchain.images.resize(imageCount);
        for (auto& image : swapchain.images)
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        if (!Check(xrEnumerateSwapchainImages(swapchain.handle, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data())),
            "xrEnumerateSwapchainImages(list)"))
            return false;
        {
            std::ostringstream imageInfo;
            imageInfo << (eye == 0 ? "Left" : "Right") << " eye image count=" << imageCount;
            logging::Info(imageInfo.str());
        }
        swapchain.rtvs.resize(imageCount);
        logging::Info(eye == 0 ? "Creating left-eye render targets" : "Creating right-eye render targets");
        D3D11_RENDER_TARGET_VIEW_DESC rtvDescription{};
        rtvDescription.Format = selected;
        rtvDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDescription.Texture2D.MipSlice = 0;
        for (std::size_t index = 0; index < imageCount; ++index)
        {
            std::ostringstream imageInfo;
            imageInfo << "Creating RTV eye=" << eye << " image=" << index
                      << " texture=" << swapchain.images[index].texture;
            logging::Info(imageInfo.str());
            const HRESULT rtvResult = device_->CreateRenderTargetView(
                swapchain.images[index].texture, &rtvDescription, &swapchain.rtvs[index]);
            if (FAILED(rtvResult))
            {
                std::ostringstream error;
                error << "CreateRenderTargetView failed HRESULT=0x" << std::hex << rtvResult;
                logging::Error(error.str());
                return false;
            }
        }
    }
    logging::Info("Stereo swapchains are ready");
    return true;
}

bool OpenXrHost::FinishInitialization(ID3D11Device* device)
{
    device_ = device;
    device_->GetImmediateContext(&context_);
    logging::Info("Creating OpenXR session");
    if (!CreateSession())
        return false;
    logging::Info("Creating OpenXR reference spaces");
    if (!CreateSpaces())
        return false;
    logging::Info("Creating OpenXR eye swapchains");
    if (!CreateSwapchains())
        return false;
    debugFrameSource_ = std::make_unique<DebugFrameSource>(context_.Get());
    if (gameSwapChain_)
        gameFrameSource_ = std::make_unique<DayZFrameSource>(gameSwapChain_.Get(), device_.Get(),
            context_.Get());
    initialized_ = true;
    logging::Info("OpenXR host initialized");
    return true;
}

void OpenXrHost::AttachGameSwapChain(IDXGISwapChain* swapChain) noexcept
{
    if (!swapChain)
        return;
    std::scoped_lock lock(mutex_);
    if (!gameSwapChain_)
        gameSwapChain_ = swapChain;
}

bool OpenXrHost::InitializeWithDevice(ID3D11Device* device) noexcept
{
    std::scoped_lock lock(mutex_);
    if (initialized_)
        return true;
    logging::Initialize();
    if (!IsOpenXrEnabled())
    {
        logging::Info("OpenXR is disabled by dayz_openxr.ini");
        return false;
    }
    if (!device || !CreateInstanceAndSystem() || !ValidateDevice(device) || !FinishInitialization(device))
    {
        logging::Error("OpenXR initialization skipped; the application will continue without VR");
        return false;
    }
    return true;
}

bool OpenXrHost::InitializeStandalone() noexcept
{
    std::scoped_lock lock(mutex_);
    if (initialized_)
        return true;
    logging::Initialize(L"xr_probe.log");
    logging::Info("xr_probe initialization started");
    if (!CreateInstanceAndSystem() || !CreateCompatibleDevice() || !FinishInitialization(device_.Get()))
        return false;
    return true;
}

void OpenXrHost::PollEvents()
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            sessionState_ = changed->state;
            std::ostringstream message;
            message << "XrSessionState = " << static_cast<int>(sessionState_);
            logging::Info(message.str());
            if (sessionState_ == XR_SESSION_STATE_READY && !sessionRunning_)
            {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                sessionRunning_ = Check(xrBeginSession(session_, &begin), "xrBeginSession");
            }
            else if (sessionState_ == XR_SESSION_STATE_STOPPING && sessionRunning_)
            {
                Check(xrEndSession(session_), "xrEndSession");
                sessionRunning_ = false;
            }
            else if (sessionState_ == XR_SESSION_STATE_EXITING ||
                sessionState_ == XR_SESSION_STATE_LOSS_PENDING)
                shouldExit_ = true;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void OpenXrHost::RenderFrame()
{
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (!Check(xrWaitFrame(session_, &waitInfo, &frameState), "xrWaitFrame"))
        return;
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (!Check(xrBeginFrame(session_, &beginInfo), "xrBeginFrame"))
        return;

    std::array<XrCompositionLayerProjectionView, 2> projectionViews{{
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
    std::uint32_t viewCount{};
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = frameState.predictedDisplayTime;
    locate.space = localSpace_;
    const bool located = Check(xrLocateViews(session_, &locate, &viewState,
        static_cast<std::uint32_t>(views_.size()), &viewCount, views_.data()), "xrLocateViews") && viewCount == 2;

    if (frameState.shouldRender && located)
    {
        dayz::stereo_state::UpdateEyePositions(
            views_[0].pose.position.x, views_[0].pose.position.y, views_[0].pose.position.z,
            views_[1].pose.position.x, views_[1].pose.position.y, views_[1].pose.position.z);
        const auto& orientation = views_[0].pose.orientation;
        dayz::stereo_state::UpdateHmdOrientation(orientation.x, orientation.y,
            orientation.z, orientation.w);
        if (gameFrameSource_)
            gameFrameSource_->PrepareFrame(dayz::stereo_state::RenderedEye());
        static std::uint64_t logCounter{};
        static auto lastLog = std::chrono::steady_clock::now();
        if (++logCounter % 120 == 0)
        {
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - lastLog).count();
            lastLog = now;
            const auto& q = views_[0].pose.orientation;
            const float sinPitch = std::clamp(2.0f * (q.w * q.x - q.z * q.y), -1.0f, 1.0f);
            const float pitch = std::asin(sinPitch);
            const float yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
            const float roll = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));
            std::ostringstream pose;
            pose << "pose q=(" << q.x << ',' << q.y << ',' << q.z << ',' << q.w
                 << ") ypr=(" << yaw << ',' << pitch << ',' << roll << ") fps="
                 << (seconds > 0.0 ? 120.0 / seconds : 0.0)
                 << " state=" << static_cast<int>(sessionState_);
            logging::Info(pose.str());
        }
        for (std::size_t eye = 0; eye < eyeSwapchains_.size(); ++eye)
        {
            auto& swapchain = eyeSwapchains_[eye];
            std::uint32_t imageIndex{};
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (!Check(xrAcquireSwapchainImage(swapchain.handle, &acquire, &imageIndex),
                "xrAcquireSwapchainImage"))
                continue;
            XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            imageWait.timeout = XR_INFINITE_DURATION;
            const bool ready = Check(xrWaitSwapchainImage(swapchain.handle, &imageWait),
                "xrWaitSwapchainImage");
            if (ready)
            {
                EyeRenderInfo renderInfo{};
                renderInfo.eyeIndex = static_cast<std::uint32_t>(eye);
                renderInfo.pose = views_[eye].pose;
                renderInfo.fov = views_[eye].fov;
                renderInfo.target = swapchain.images[imageIndex].texture;
                renderInfo.rtv = swapchain.rtvs[imageIndex].Get();
                renderInfo.width = swapchain.width;
                renderInfo.height = swapchain.height;
                if (gameFrameSource_ && gameFrameSource_->HasGameData())
                    gameFrameSource_->RenderEye(renderInfo);
                else
                    debugFrameSource_->RenderEye(renderInfo);
            }
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            Check(xrReleaseSwapchainImage(swapchain.handle, &release), "xrReleaseSwapchainImage");

            auto& layerView = projectionViews[eye];
            layerView.pose = views_[eye].pose;
            layerView.fov = views_[eye].fov;
            layerView.subImage.swapchain = swapchain.handle;
            layerView.subImage.imageRect.extent = {
                static_cast<std::int32_t>(swapchain.width), static_cast<std::int32_t>(swapchain.height)};
            layerView.subImage.imageArrayIndex = 0;
        }
    }

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = localSpace_;
    layer.viewCount = static_cast<std::uint32_t>(projectionViews.size());
    layer.views = projectionViews.data();
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = frameState.shouldRender && located ? 1u : 0u;
    endInfo.layers = endInfo.layerCount ? layers : nullptr;
    Check(xrEndFrame(session_, &endInfo), "xrEndFrame");
}

void OpenXrHost::Tick() noexcept
{
    std::scoped_lock lock(mutex_);
    if (!initialized_)
        return;
    PollEvents();
    if (sessionRunning_)
        RenderFrame();
}

void OpenXrHost::Shutdown() noexcept
{
    std::scoped_lock lock(mutex_);
    gameFrameSource_.reset();
    debugFrameSource_.reset();
    gameSwapChain_.Reset();
    for (auto& swapchain : eyeSwapchains_)
    {
        swapchain.rtvs.clear();
        swapchain.images.clear();
        if (swapchain.handle != XR_NULL_HANDLE)
            xrDestroySwapchain(swapchain.handle);
        swapchain.handle = XR_NULL_HANDLE;
    }
    if (viewSpace_ != XR_NULL_HANDLE) xrDestroySpace(viewSpace_);
    if (localSpace_ != XR_NULL_HANDLE) xrDestroySpace(localSpace_);
    if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
    if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
    viewSpace_ = localSpace_ = XR_NULL_HANDLE;
    session_ = XR_NULL_HANDLE;
    instance_ = XR_NULL_HANDLE;
    context_.Reset();
    device_.Reset();
    initialized_ = sessionRunning_ = false;
}
