#include "openxr_host.hpp"

#include "debug_frame_source.hpp"
#include "dayz_frame_source.hpp"
#include "dayz_runtime_probe.hpp"
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

    bool ReadBoolean(const wchar_t* section, const wchar_t* key, bool fallback) noexcept
    {
        wchar_t value[16]{};
        GetPrivateProfileStringW(section, key, fallback ? L"true" : L"false", value,
            static_cast<DWORD>(std::size(value)), ConfigurationPath().c_str());
        return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
            _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
    }

    float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback) noexcept
    {
        wchar_t fallbackText[32]{};
        swprintf_s(fallbackText, L"%.3f", fallback);
        wchar_t value[32]{};
        GetPrivateProfileStringW(section, key, fallbackText, value,
            static_cast<DWORD>(std::size(value)), ConfigurationPath().c_str());
        wchar_t* end{};
        const float parsed = std::wcstof(value, &end);
        return end != value && std::isfinite(parsed) ? parsed : fallback;
    }

    std::uint32_t ReadUnsigned(const wchar_t* section, const wchar_t* key,
        std::uint32_t fallback) noexcept
    {
        return static_cast<std::uint32_t>((std::max)(1u, GetPrivateProfileIntW(section,
            key, static_cast<int>(fallback), ConfigurationPath().c_str())));
    }

    XrQuaternionf YawOnly(const XrQuaternionf& orientation) noexcept
    {
        const float yaw = std::atan2(2.0f * (orientation.w * orientation.y +
            orientation.x * orientation.z), 1.0f - 2.0f *
            (orientation.x * orientation.x + orientation.y * orientation.y));
        const float half = yaw * 0.5f;
        return {0.0f, std::sin(half), 0.0f, std::cos(half)};
    }

    XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) noexcept
    {
        return {
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }

    XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) noexcept
    {
        const XrQuaternionf p{v.x, v.y, v.z, 0.0f};
        const XrQuaternionf inverse{-q.x, -q.y, -q.z, q.w};
        const XrQuaternionf result = Multiply(Multiply(q, p), inverse);
        return {result.x, result.y, result.z};
    }

    void SendKey(WORD key, bool down) noexcept
    {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
        input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
        SendInput(1, &input, sizeof(input));
    }

    void SendMouseButton(bool down) noexcept
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        SendInput(1, &input, sizeof(input));
    }

    void SendMouseTurn(LONG x) noexcept
    {
        if (!x)
            return;
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = x;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &input, sizeof(input));
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

bool OpenXrHost::CreateControllerActions()
{
    if (!controllerInputEnabled_)
        return true;
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(setInfo.actionSetName, "dayz_vr_controls");
    strcpy_s(setInfo.localizedActionSetName, "DayZ VR Controls");
    setInfo.priority = 0;
    if (!Check(xrCreateActionSet(instance_, &setInfo, &actionSet_), "xrCreateActionSet"))
        return false;
    if (!Check(xrStringToPath(instance_, "/user/hand/left", &handPaths_[0]),
            "xrStringToPath(left hand)") ||
        !Check(xrStringToPath(instance_, "/user/hand/right", &handPaths_[1]),
            "xrStringToPath(right hand)"))
        return false;

    const auto createAction = [&](const char* name, const char* localized,
        XrActionType type, XrAction& action) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(info.actionName, name);
        strcpy_s(info.localizedActionName, localized);
        info.actionType = type;
        info.countSubactionPaths = static_cast<std::uint32_t>(handPaths_.size());
        info.subactionPaths = handPaths_.data();
        return Check(xrCreateAction(actionSet_, &info, &action), "xrCreateAction");
    };
    if (!createAction("grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT,
            gripPoseAction_) ||
        !createAction("aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT,
            aimPoseAction_) ||
        !createAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT,
            triggerAction_) ||
        !createAction("x_button", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT,
            xButtonAction_) ||
        !createAction("y_button", "Inventory", XR_ACTION_TYPE_BOOLEAN_INPUT,
            yButtonAction_) ||
        !createAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT,
            thumbstickAction_))
        return false;

    const auto path = [&](const char* text) {
        XrPath result{XR_NULL_PATH};
        xrStringToPath(instance_, text, &result);
        return result;
    };
    const auto suggest = [&](const char* profile,
        const std::vector<XrActionSuggestedBinding>& bindings) {
        XrInteractionProfileSuggestedBinding info{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        info.interactionProfile = path(profile);
        info.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
        info.suggestedBindings = bindings.data();
        const XrResult result = xrSuggestInteractionProfileBindings(instance_, &info);
        if (XR_FAILED(result))
            logging::XrError("xrSuggestInteractionProfileBindings", result);
    };
    suggest("/interaction_profiles/oculus/touch_controller", {
        {gripPoseAction_, path("/user/hand/left/input/grip/pose")},
        {gripPoseAction_, path("/user/hand/right/input/grip/pose")},
        {aimPoseAction_, path("/user/hand/left/input/aim/pose")},
        {aimPoseAction_, path("/user/hand/right/input/aim/pose")},
        {triggerAction_, path("/user/hand/right/input/trigger/value")},
        {xButtonAction_, path("/user/hand/left/input/x/click")},
        {yButtonAction_, path("/user/hand/left/input/y/click")},
        {thumbstickAction_, path("/user/hand/left/input/thumbstick")},
        {thumbstickAction_, path("/user/hand/right/input/thumbstick")}});
    suggest("/interaction_profiles/valve/index_controller", {
        {gripPoseAction_, path("/user/hand/left/input/grip/pose")},
        {gripPoseAction_, path("/user/hand/right/input/grip/pose")},
        {aimPoseAction_, path("/user/hand/left/input/aim/pose")},
        {aimPoseAction_, path("/user/hand/right/input/aim/pose")},
        {triggerAction_, path("/user/hand/right/input/trigger/value")},
        {xButtonAction_, path("/user/hand/left/input/a/click")},
        {yButtonAction_, path("/user/hand/left/input/b/click")},
        {thumbstickAction_, path("/user/hand/left/input/thumbstick")},
        {thumbstickAction_, path("/user/hand/right/input/thumbstick")}});
    const auto suggestXyController = [&](const char* profile) {
        suggest(profile, {
            {gripPoseAction_, path("/user/hand/left/input/grip/pose")},
            {gripPoseAction_, path("/user/hand/right/input/grip/pose")},
            {aimPoseAction_, path("/user/hand/left/input/aim/pose")},
            {aimPoseAction_, path("/user/hand/right/input/aim/pose")},
            {triggerAction_, path("/user/hand/right/input/trigger/value")},
            {xButtonAction_, path("/user/hand/left/input/x/click")},
            {yButtonAction_, path("/user/hand/left/input/y/click")},
            {thumbstickAction_, path("/user/hand/left/input/thumbstick")},
            {thumbstickAction_, path("/user/hand/right/input/thumbstick")}});
    };
    suggestXyController("/interaction_profiles/facebook/touch_controller_pro");
    suggestXyController("/interaction_profiles/meta/touch_controller_plus");
    suggestXyController("/interaction_profiles/bytedance/pico_neo3_controller");
    suggestXyController("/interaction_profiles/htc/vive_cosmos_controller");
    suggestXyController("/interaction_profiles/hp/mixed_reality_controller");

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet_;
    if (!Check(xrAttachSessionActionSets(session_, &attach), "xrAttachSessionActionSets"))
        return false;
    for (std::size_t hand = 0; hand < handPaths_.size(); ++hand)
    {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        spaceInfo.subactionPath = handPaths_[hand];
        spaceInfo.action = gripPoseAction_;
        if (!Check(xrCreateActionSpace(session_, &spaceInfo, &gripSpaces_[hand]),
                "xrCreateActionSpace(grip)"))
            return false;
        spaceInfo.action = aimPoseAction_;
        if (!Check(xrCreateActionSpace(session_, &spaceInfo, &aimSpaces_[hand]),
                "xrCreateActionSpace(aim)"))
            return false;
    }
    logging::Info("OpenXR controller actions ready");
    return true;
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
    if (guiQuadEnabled_ && !CreateGuiSwapchain(formats))
    {
        logging::Error("GUI quad swapchain is unavailable; projection rendering will continue");
        guiQuadEnabled_ = false;
    }
    if (controllerAxesEnabled_ && !CreateAxisSwapchain(formats))
    {
        logging::Error("Controller axis swapchain unavailable; controller input will continue");
        controllerAxesEnabled_ = false;
    }
    logging::Info("Stereo swapchains are ready");
    return true;
}

bool OpenXrHost::CreateGuiSwapchain(const std::vector<std::int64_t>& formats)
{
    const DXGI_FORMAT preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    DXGI_FORMAT selected = DXGI_FORMAT_UNKNOWN;
    for (const auto candidate : preferred)
        if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(candidate)) !=
            formats.end())
        {
            selected = candidate;
            break;
        }
    if (selected == DXGI_FORMAT_UNKNOWN)
        return false;

    guiSwapchain_.width = (std::clamp)(ReadUnsigned(L"gui", L"quad_pixel_width", 1920),
        512u, 4096u);
    guiSwapchain_.height = (std::clamp)(ReadUnsigned(L"gui", L"quad_pixel_height", 1400),
        512u, 4096u);
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = selected;
    info.sampleCount = 1;
    info.width = guiSwapchain_.width;
    info.height = guiSwapchain_.height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Check(xrCreateSwapchain(session_, &info, &guiSwapchain_.handle),
        "xrCreateSwapchain(gui)"))
        return false;

    std::uint32_t imageCount{};
    if (!Check(xrEnumerateSwapchainImages(guiSwapchain_.handle, 0, &imageCount, nullptr),
        "xrEnumerateSwapchainImages(gui count)"))
        return false;
    guiSwapchain_.images.resize(imageCount);
    for (auto& image : guiSwapchain_.images)
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    if (!Check(xrEnumerateSwapchainImages(guiSwapchain_.handle, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(guiSwapchain_.images.data())),
        "xrEnumerateSwapchainImages(gui list)"))
        return false;

    guiSwapchain_.rtvs.resize(imageCount);
    D3D11_RENDER_TARGET_VIEW_DESC rtvDescription{};
    rtvDescription.Format = selected;
    rtvDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    for (std::size_t index = 0; index < imageCount; ++index)
        if (FAILED(device_->CreateRenderTargetView(guiSwapchain_.images[index].texture,
            &rtvDescription, &guiSwapchain_.rtvs[index])))
            return false;

    std::ostringstream message;
    message << "GUI quad swapchain ready: " << guiSwapchain_.width << 'x'
        << guiSwapchain_.height << " images=" << imageCount;
    logging::Info(message.str());
    return true;
}

bool OpenXrHost::CreateAxisSwapchain(const std::vector<std::int64_t>& formats)
{
    if (!controllerAxesEnabled_)
        return true;
    const DXGI_FORMAT format = std::find(formats.begin(), formats.end(),
        static_cast<std::int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM)) != formats.end()
        ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(format)) ==
        formats.end())
        return false;
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = 3;
    info.height = 1;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Check(xrCreateSwapchain(session_, &info, &axisSwapchain_.handle),
            "xrCreateSwapchain(controller axes)"))
        return false;
    std::uint32_t count{};
    if (!Check(xrEnumerateSwapchainImages(axisSwapchain_.handle, 0, &count, nullptr),
            "xrEnumerateSwapchainImages(controller axes count)"))
        return false;
    axisSwapchain_.images.resize(count);
    for (auto& image : axisSwapchain_.images)
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    if (!Check(xrEnumerateSwapchainImages(axisSwapchain_.handle, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(axisSwapchain_.images.data())),
            "xrEnumerateSwapchainImages(controller axes)"))
        return false;
    const std::uint32_t pixels[3]{0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u};
    for (const auto& image : axisSwapchain_.images)
        context_->UpdateSubresource(image.texture, 0, nullptr, pixels, sizeof(pixels), 0);
    logging::Info("Controller XYZ axis swapchain ready");
    return true;
}

bool OpenXrHost::FinishInitialization(ID3D11Device* device)
{
    device_ = device;
    device_->GetImmediateContext(&context_);
    guiQuadEnabled_ = ReadBoolean(L"gui", L"quad_enabled", true);
    guiQuadWidthMeters_ = (std::clamp)(ReadFloat(L"gui", L"quad_width_meters", 1.4f),
        0.4f, 4.0f);
    guiQuadDistance_ = (std::clamp)(ReadFloat(L"gui", L"quad_distance", 1.25f),
        0.4f, 5.0f);
    guiQuadVerticalOffset_ = (std::clamp)(ReadFloat(L"gui", L"quad_vertical_offset", -0.1f),
        -2.0f, 2.0f);
    controllerInputEnabled_ = ReadBoolean(L"controls", L"enabled", true);
    controllerAxesEnabled_ = ReadBoolean(L"controls", L"show_controller_axes", true);
    controllerAxesEnabled_ = controllerAxesEnabled_ && controllerInputEnabled_;
    controllerTurnScale_ = ReadFloat(L"controls", L"turn_scale", 18.0f);
    controllerDeadzone_ = (std::clamp)(ReadFloat(L"controls", L"deadzone", 0.3f),
        0.0f, 0.9f);
    logging::Info("Creating OpenXR session");
    if (!CreateSession())
        return false;
    if (!CreateControllerActions())
    {
        logging::Error("OpenXR controller actions unavailable; headset rendering will continue");
        controllerInputEnabled_ = false;
        controllerAxesEnabled_ = false;
    }
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
                ReleaseControllerKeys();
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

void OpenXrHost::AnchorGuiQuad(const XrPosef& headPose) noexcept
{
    const XrQuaternionf yaw = YawOnly(headPose.orientation);
    const XrVector3f forward{
        -2.0f * (yaw.x * yaw.z + yaw.w * yaw.y),
        -2.0f * (yaw.y * yaw.z - yaw.w * yaw.x),
        -(1.0f - 2.0f * (yaw.x * yaw.x + yaw.y * yaw.y))};
    guiQuadPose_.orientation = yaw;
    guiQuadPose_.position = {
        headPose.position.x + forward.x * guiQuadDistance_,
        headPose.position.y + forward.y * guiQuadDistance_ + guiQuadVerticalOffset_,
        headPose.position.z + forward.z * guiQuadDistance_};
    guiQuadAnchored_ = true;
    logging::Info("GUI quad anchored in LOCAL space");
}

void OpenXrHost::ReleaseControllerKeys() noexcept
{
    constexpr WORD keys[4]{'W', 'A', 'S', 'D'};
    for (std::size_t index = 0; index < movementKeys_.size(); ++index)
        if (movementKeys_[index])
        {
            SendKey(keys[index], false);
            movementKeys_[index] = false;
        }
    if (triggerDown_)
    {
        SendMouseButton(false);
        triggerDown_ = false;
    }
    if (xButtonDown_)
    {
        SendKey(VK_ESCAPE, false);
        xButtonDown_ = false;
    }
    if (yButtonDown_)
    {
        SendKey(VK_TAB, false);
        yButtonDown_ = false;
    }
}

void OpenXrHost::SyncControllerInput(XrTime displayTime, bool guiVisible)
{
    if (!controllerInputEnabled_ || actionSet_ == XR_NULL_HANDLE)
        return;
    XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (!Check(xrSyncActions(session_, &sync), "xrSyncActions"))
        return;
    static XrPath loggedInteractionProfile{XR_NULL_PATH};
    XrInteractionProfileState interaction{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session_, handPaths_[0], &interaction)) &&
        interaction.interactionProfile != XR_NULL_PATH &&
        interaction.interactionProfile != loggedInteractionProfile)
    {
        char profile[XR_MAX_PATH_LENGTH]{};
        std::uint32_t length{};
        if (XR_SUCCEEDED(xrPathToString(instance_, interaction.interactionProfile,
                static_cast<std::uint32_t>(std::size(profile)), &length, profile)))
        {
            logging::Info(std::string("OpenXR left controller profile: ") + profile);
            loggedInteractionProfile = interaction.interactionProfile;
        }
    }
    for (std::size_t hand = 0; hand < handPaths_.size(); ++hand)
    {
        gripLocations_[hand] = {XR_TYPE_SPACE_LOCATION};
        aimLocations_[hand] = {XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(gripSpaces_[hand], localSpace_, displayTime, &gripLocations_[hand]);
        xrLocateSpace(aimSpaces_[hand], localSpace_, displayTime, &aimLocations_[hand]);
    }
#ifndef _WINDLL
    (void)guiVisible;
    return;
#endif
    const auto vectorState = [&](std::size_t hand) {
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = thumbstickAction_;
        get.subactionPath = handPaths_[hand];
        xrGetActionStateVector2f(session_, &get, &state);
        return state.isActive ? state.currentState : XrVector2f{};
    };
    const XrVector2f leftStick = vectorState(0);
    const XrVector2f rightStick = vectorState(1);
    const bool desired[4]{leftStick.y > controllerDeadzone_,
        leftStick.x < -controllerDeadzone_, leftStick.y < -controllerDeadzone_,
        leftStick.x > controllerDeadzone_};
    constexpr WORD keys[4]{'W', 'A', 'S', 'D'};
    for (std::size_t index = 0; index < movementKeys_.size(); ++index)
        if (movementKeys_[index] != desired[index])
        {
            SendKey(keys[index], desired[index]);
            movementKeys_[index] = desired[index];
        }
    if (std::fabs(rightStick.x) > controllerDeadzone_)
        SendMouseTurn(static_cast<LONG>(std::lround(rightStick.x * controllerTurnScale_)));

    const auto booleanState = [&](XrAction action, std::size_t hand) {
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = action;
        get.subactionPath = handPaths_[hand];
        xrGetActionStateBoolean(session_, &get, &state);
        return state;
    };
    const XrActionStateBoolean xState = booleanState(xButtonAction_, 0);
    const XrActionStateBoolean yState = booleanState(yButtonAction_, 0);
    const bool xDown = xState.isActive && xState.currentState;
    const bool yDown = yState.isActive && yState.currentState;
    if (xDown != xButtonDown_)
        SendKey(VK_ESCAPE, xDown);
    if (yDown != yButtonDown_)
        SendKey(VK_TAB, yDown);
    xButtonDown_ = xDown;
    yButtonDown_ = yDown;

    XrActionStateFloat trigger{XR_TYPE_ACTION_STATE_FLOAT};
    XrActionStateGetInfo triggerGet{XR_TYPE_ACTION_STATE_GET_INFO};
    triggerGet.action = triggerAction_;
    triggerGet.subactionPath = handPaths_[1];
    xrGetActionStateFloat(session_, &triggerGet, &trigger);
    bool cursorHit{};
    if (guiVisible && guiQuadAnchored_ &&
        (aimLocations_[1].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
        (aimLocations_[1].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    {
        const XrPosef& aim = aimLocations_[1].pose;
        const XrVector3f ray = Rotate(aim.orientation, {0.0f, 0.0f, -1.0f});
        const XrQuaternionf inverse{-guiQuadPose_.orientation.x, -guiQuadPose_.orientation.y,
            -guiQuadPose_.orientation.z, guiQuadPose_.orientation.w};
        const XrVector3f relative{aim.position.x - guiQuadPose_.position.x,
            aim.position.y - guiQuadPose_.position.y,
            aim.position.z - guiQuadPose_.position.z};
        const XrVector3f localOrigin = Rotate(inverse, relative);
        const XrVector3f localDirection = Rotate(inverse, ray);
        if (std::fabs(localDirection.z) > 0.00001f)
        {
            const float distance = -localOrigin.z / localDirection.z;
            const float height = guiQuadWidthMeters_ *
                static_cast<float>(guiSwapchain_.height) /
                static_cast<float>((std::max)(1u, guiSwapchain_.width));
            const float x = localOrigin.x + localDirection.x * distance;
            const float y = localOrigin.y + localDirection.y * distance;
            if (distance > 0.0f && std::fabs(x) <= guiQuadWidthMeters_ * 0.5f &&
                std::fabs(y) <= height * 0.5f)
            {
                cursorHit = true;
#ifdef _WINDLL
                dayz::runtime_probe::SetGuiVirtualCursorNormalized(
                    x / guiQuadWidthMeters_ + 0.5f, 0.5f - y / height);
#endif
            }
        }
    }
    const bool desiredTrigger = guiVisible && cursorHit && trigger.isActive &&
        trigger.currentState > 0.55f;
    if (desiredTrigger != triggerDown_)
    {
        SendMouseButton(desiredTrigger);
        triggerDown_ = desiredTrigger;
    }
    static std::uint64_t inputLogCounter{};
    if (++inputLogCounter % 180 == 0)
    {
        std::ostringstream message;
        message << "controller input left=" << leftStick.x << ',' << leftStick.y
            << " right=" << rightStick.x << ',' << rightStick.y
            << " X=" << xDown << "(active=" << xState.isActive << ')'
            << " Y=" << yDown << "(active=" << yState.isActive << ')'
            << " trigger=" << trigger.currentState << " gui_hit=" << cursorHit;
        logging::Info(message.str());
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

#ifdef _WINDLL
    const bool guiVisible = guiQuadEnabled_ && guiSwapchain_.handle != XR_NULL_HANDLE &&
        dayz::runtime_probe::IsGuiQuadVisible();
#else
    const bool guiVisible = false;
#endif
    if (frameState.shouldRender && located)
    {
        dayz::stereo_state::UpdateEyePositions(
            views_[0].pose.position.x, views_[0].pose.position.y, views_[0].pose.position.z,
            views_[1].pose.position.x, views_[1].pose.position.y, views_[1].pose.position.z);
        dayz::stereo_state::UpdateHmdPosition(
            (views_[0].pose.position.x + views_[1].pose.position.x) * 0.5f,
            (views_[0].pose.position.y + views_[1].pose.position.y) * 0.5f,
            (views_[0].pose.position.z + views_[1].pose.position.z) * 0.5f);
        const auto& orientation = views_[0].pose.orientation;
        dayz::stereo_state::UpdateHmdOrientation(orientation.x, orientation.y,
            orientation.z, orientation.w);
        if (guiVisible && (!guiQuadWasVisible_ || !guiQuadAnchored_))
            AnchorGuiQuad(views_[0].pose);
        SyncControllerInput(frameState.predictedDisplayTime, guiVisible);
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

        if (guiVisible)
        {
            std::uint32_t imageIndex{};
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (Check(xrAcquireSwapchainImage(guiSwapchain_.handle, &acquire, &imageIndex),
                "xrAcquireSwapchainImage(gui)"))
            {
                XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                imageWait.timeout = XR_INFINITE_DURATION;
                if (Check(xrWaitSwapchainImage(guiSwapchain_.handle, &imageWait),
                    "xrWaitSwapchainImage(gui)"))
                {
#ifdef _WINDLL
                    guiQuadHasImage_ = dayz::runtime_probe::RenderGuiQuad(
                        guiSwapchain_.rtvs[imageIndex].Get(), guiSwapchain_.width,
                        guiSwapchain_.height) || guiQuadHasImage_;
#endif
                }
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                Check(xrReleaseSwapchainImage(guiSwapchain_.handle, &release),
                    "xrReleaseSwapchainImage(gui)");
            }
        }
    }
    guiQuadWasVisible_ = guiVisible;
    if (!guiVisible)
    {
        guiQuadAnchored_ = false;
        guiQuadHasImage_ = false;
    }

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = localSpace_;
    layer.viewCount = static_cast<std::uint32_t>(projectionViews.size());
    layer.views = projectionViews.data();
    XrCompositionLayerQuad guiLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    guiLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    guiLayer.space = localSpace_;
    guiLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    guiLayer.subImage.swapchain = guiSwapchain_.handle;
    guiLayer.subImage.imageRect.extent = {static_cast<std::int32_t>(guiSwapchain_.width),
        static_cast<std::int32_t>(guiSwapchain_.height)};
    guiLayer.pose = guiQuadPose_;
    guiLayer.size.width = guiQuadWidthMeters_;
    guiLayer.size.height = guiQuadWidthMeters_ * static_cast<float>(guiSwapchain_.height) /
        static_cast<float>((std::max)(1u, guiSwapchain_.width));
    std::array<XrCompositionLayerQuad, 6> axisLayers{};
    std::uint32_t axisLayerCount{};
    if (frameState.shouldRender && located && controllerAxesEnabled_ &&
        axisSwapchain_.handle != XR_NULL_HANDLE)
    {
        std::uint32_t imageIndex{};
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(axisSwapchain_.handle, &acquire,
                &imageIndex)))
        {
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(xrWaitSwapchainImage(axisSwapchain_.handle, &wait)))
            {
                const std::uint32_t pixels[3]{0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u};
                context_->UpdateSubresource(axisSwapchain_.images[imageIndex].texture, 0,
                    nullptr, pixels, sizeof(pixels), 0);
            }
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(axisSwapchain_.handle, &release);
        }
        constexpr float halfLength = 0.04f;
        constexpr float thickness = 0.006f;
        const XrVector3f directions[3]{{1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        constexpr float s = 0.70710678f;
        const XrQuaternionf axisRotations[3]{{0.0f, 0.0f, 0.0f, 1.0f},
            {0.0f, 0.0f, s, s}, {0.0f, -s, 0.0f, s}};
        for (std::size_t hand = 0; hand < gripLocations_.size(); ++hand)
        {
            const auto& location = gripLocations_[hand];
            if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0 ||
                (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0)
                continue;
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                XrCompositionLayerQuad& axisLayer = axisLayers[axisLayerCount++];
                axisLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                axisLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                axisLayer.space = localSpace_;
                axisLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                axisLayer.subImage.swapchain = axisSwapchain_.handle;
                axisLayer.subImage.imageRect.offset = {static_cast<std::int32_t>(axis), 0};
                axisLayer.subImage.imageRect.extent = {1, 1};
                const XrVector3f direction = Rotate(location.pose.orientation,
                    directions[axis]);
                axisLayer.pose.position = {
                    location.pose.position.x + direction.x * halfLength,
                    location.pose.position.y + direction.y * halfLength,
                    location.pose.position.z + direction.z * halfLength};
                axisLayer.pose.orientation = Multiply(location.pose.orientation,
                    axisRotations[axis]);
                axisLayer.size = {halfLength * 2.0f, thickness};
            }
        }
    }
    std::array<const XrCompositionLayerBaseHeader*, 8> layers{};
    std::uint32_t layerCount = frameState.shouldRender && located ? 1u : 0u;
    if (layerCount)
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    for (std::uint32_t index = 0; index < axisLayerCount && layerCount < layers.size(); ++index)
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(
            &axisLayers[index]);
    if (layerCount && guiVisible && guiQuadHasImage_)
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&guiLayer);
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount ? layers.data() : nullptr;
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
    ReleaseControllerKeys();
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
    guiSwapchain_.rtvs.clear();
    guiSwapchain_.images.clear();
    if (guiSwapchain_.handle != XR_NULL_HANDLE)
        xrDestroySwapchain(guiSwapchain_.handle);
    guiSwapchain_.handle = XR_NULL_HANDLE;
    axisSwapchain_.images.clear();
    if (axisSwapchain_.handle != XR_NULL_HANDLE)
        xrDestroySwapchain(axisSwapchain_.handle);
    axisSwapchain_.handle = XR_NULL_HANDLE;
    for (XrSpace& space : aimSpaces_)
        if (space != XR_NULL_HANDLE) xrDestroySpace(space);
    for (XrSpace& space : gripSpaces_)
        if (space != XR_NULL_HANDLE) xrDestroySpace(space);
    aimSpaces_.fill(XR_NULL_HANDLE);
    gripSpaces_.fill(XR_NULL_HANDLE);
    if (actionSet_ != XR_NULL_HANDLE)
        xrDestroyActionSet(actionSet_);
    actionSet_ = XR_NULL_HANDLE;
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
