#include "Context.hpp"
#include <format>
#include <iostream>
#include <print>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <webgpu/webgpu_glfw.h>
#include <unordered_map>
#include <algorithm>
#include "magic_enum/magic_enum.hpp"

#if !defined(__EMSCRIPTEN__) && defined(_WIN32)
#undef APIENTRY
#define WIN32_LEAN_AND_MEAN
#include <dawn/native/DawnNative.h>
#include <windows.h>
// :( 
std::string GetSystemDirectory()
{
    char buffer[MAX_PATH];
    UINT result = GetSystemDirectoryA(buffer, MAX_PATH);
    if (result == 0 || result > MAX_PATH)
    {
        std::println(stderr, "Failed to get system directory: {}", GetLastError());
        return std::string{};
    }
    // path has to end with //, as dawn won't append it automatically lol
    std::string strResult(buffer);
    if (strResult.back() != '\\')
    {
        strResult += '\\';
    }
    return strResult;
}
#endif

namespace
{
    

    // todo: we should sink these somewhere more portable, and which could actually give us debug info in live clients maybe?
    [[noreturn]] void LogUncapturedError(
        [[maybe_unused]] const wgpu::Device&,
        wgpu::ErrorType type,
        wgpu::StringView message)
    {
        std::println(stderr,
                     "[wgpu] Uncaptured error, exiting | Error Type \"{}\" | Message: {}",
                     magic_enum::enum_name(type), std::string_view(message.data, message.length));
        std::exit(1);
    }

    void LogDeviceLost(
        [[maybe_unused]] const wgpu::Device&,
        wgpu::DeviceLostReason reason,
        wgpu::StringView message)
    {
        // note that this is also called for routine destruction, so messages from here don't always mean something went wrong
        std::println(stderr,
                     "[wgpu] Device lost | Reason: \"{}\" | Message: {}",
                     magic_enum::enum_name(reason), std::string_view(message.data, message.length));
    }
}

namespace velox
{

Context::Context(const ContextCreateInfo& createInfo)
{
    instance = ValidOrExit(requestInstance(createInfo));

    adapter = ValidOrExit(requestAdapter(createInfo));
    device = ValidOrExit(requestDevice(createInfo));
    queue = device.GetQueue();
    std::println(stderr, "[velox][context] Instance, Adapter, and Device online");
    std::string enabledFeatureNames;
    for (const auto& feature : createInfo.RequiredFeatures)
    {
        enabledFeatureNames += std::format(" {} |", magic_enum::enum_name(feature));
    }
    std::println(stderr, "[velox][context] Device enabled features:{}", enabledFeatureNames);

    nativeWindow = ValidOrExit(createNativeWindow(createInfo));
    surface = ValidOrExit(createSurface(createInfo));
    configureSurface(createInfo);
}

Context::~Context()
{
    glfwDestroyWindow(nativeWindow);
    glfwTerminate();
}



ResizeStatus Context::Resize(uint32_t width, uint32_t height)
{
    if (width == surfaceConfig.width && height == surfaceConfig.height)
    {
        return ResizeStatus::Unchanged;
    }
    else if (width == 0 || height == 0)
    {
        return ResizeStatus::Minimized;
    }
    else
    {
        surfaceConfig.width = width;
        surfaceConfig.height = height;
        surface.Configure(&surfaceConfig);
        return ResizeStatus::Resized;
    }
}

wgpu::TextureView Context::AcquireNextFrame()
{
    wgpu::SurfaceTexture surfaceTexture{};
    surface.GetCurrentTexture(&surfaceTexture);

    switch (surfaceTexture.status)
    {
    case wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal:
        return surfaceTexture.texture.CreateView();
    case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
        std::println(stderr, "[velox][context] Next frame acquisition returned SuccessSuboptimal");
        return surfaceTexture.texture.CreateView();
    default:
        // todo: this should be a std::expected return
        return wgpu::TextureView{};
    }
}

void Context::Present()
{
#ifndef __EMSCRIPTEN__
    // Emscripten presents implicitly at the end of each browser frame;
    // calling Present() there is a validation error.
    surface.Present();
#endif
}

wgpu::Instance& Context::GetInstance() noexcept
{
    return instance;
}

wgpu::Adapter& Context::GetAdapter() noexcept
{
    return adapter;
}

wgpu::Device& Context::GetDevice() noexcept
{
    return device;
}

wgpu::Queue& Context::GetQueue() noexcept
{
    return queue;
}

wgpu::Surface& Context::GetSurface() noexcept
{
    return surface;
}

wgpu::TextureFormat Context::GetSurfaceFormat() const noexcept
{
    return surfaceConfig.format;
}

bool Context::HasFeature(wgpu::FeatureName feature) const noexcept
{
    return device.HasFeature(feature);
}

#ifndef __EMSCRIPTEN__
GLFWwindow* Context::GetNativeWindow() const noexcept
{
    return nativeWindow;
}
#endif

std::expected<wgpu::Instance, RhiError> Context::requestInstance(const ContextCreateInfo& createInfo)
{
    wgpu::InstanceDescriptor instanceDesc{};
    const wgpu::InstanceFeatureName requiredFeatures[] = { wgpu::InstanceFeatureName::TimedWaitAny };
    instanceDesc.requiredFeatureCount = std::size(requiredFeatures);
    instanceDesc.requiredFeatures = requiredFeatures;
#if !defined(__EMSCRIPTEN__) && defined(_WIN32)
    // as mentioned above, we want to make sure Dawn can find vulkan-1.dll on windows
    // (only when we're compiling for Native on Win32, of course!)
    std::string sys32Path = GetSystemDirectory();
    const char* searchPaths[] = { sys32Path.c_str() };

    dawn::native::DawnInstanceDescriptor dawnDescriptor{};
    dawnDescriptor.additionalRuntimeSearchPathsCount = std::size(searchPaths);
    dawnDescriptor.additionalRuntimeSearchPaths = searchPaths;
    instanceDesc.nextInChain = &dawnDescriptor;
#else
    instanceDesc.nextInChain = nullptr;
#endif
    instance = wgpu::CreateInstance(&instanceDesc);
    if (!instance)
    {
        return std::unexpected(RhiError::InstanceRequestFailed);
    }
    return std::move(instance);
}

std::expected<wgpu::Adapter, RhiError> Context::requestAdapter(const ContextCreateInfo& createInfo)
{
    wgpu::RequestAdapterOptions options{};
#ifndef __EMSCRIPTEN__
    // can't use vulkan on desktop, as nvidia drivers have severe bugs with f16
    // our entire framework is built on f16 LMAO
    // https://issues.chromium.org/issues/42251215
    options.backendType = wgpu::BackendType::D3D12;
#endif
    options.featureLevel = createInfo.FeatureLevel;
    options.powerPreference = createInfo.PowerPreference;

    wgpu::Adapter result_adapter;
    wgpu::Future future = instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
        [&result_adapter](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView message)
        {
            if (status == wgpu::RequestAdapterStatus::Success)
            {
                result_adapter = std::move(result);
            }
            else
            {
                result_adapter = wgpu::Adapter{}; // ensure it's empty
                std::println(stderr, "[velox][context] RequestAdapter failed: {}", std::string_view(message.data, message.length));
            }
        });

    instance.WaitAny(future, UINT64_MAX);
    
    if (!result_adapter)
    {
        return std::unexpected(RhiError::AdapterRequestFailed);
    }

    return result_adapter;
}

std::expected<wgpu::Device, RhiError> Context::requestDevice(const ContextCreateInfo& createInfo)
{
    wgpu::DeviceDescriptor deviceDesc{};
    deviceDesc.label = createInfo.ApplicationName;
    deviceDesc.SetUncapturedErrorCallback(LogUncapturedError);
    deviceDesc.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous, LogDeviceLost);
    if (!createInfo.RequiredFeatures.empty())
    {
        deviceDesc.requiredFeatureCount = createInfo.RequiredFeatures.size();
        deviceDesc.requiredFeatures = createInfo.RequiredFeatures.data();
    }
    else
    {
        deviceDesc.requiredFeatureCount = 0;
        deviceDesc.requiredFeatures = nullptr;
    }

    wgpu::Device result_device;
    wgpu::Future future = adapter.RequestDevice(&deviceDesc, wgpu::CallbackMode::WaitAnyOnly,
        [&result_device](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView message)
        {
            if (status == wgpu::RequestDeviceStatus::Success)
            {
                result_device = std::move(result);
            }
            else
            {
                result_device = wgpu::Device{}; // ensure it's empty
                std::println(stderr, "[velox][context] RequestDevice failed: {}", std::string_view(message.data, message.length));
            }
        });

    instance.WaitAny(future, UINT64_MAX);

    if (!result_device)
    {
        return std::unexpected(RhiError::DeviceRequestFailed);
    }

    return result_device;
}

std::expected<GLFWwindow*, RhiError> Context::createNativeWindow(const ContextCreateInfo& createInfo)
{
    // todo: We have a bunch of nice example code for how to set backbuffer bit depth and color stuff in DiamondDogs,
    // along with configuring other parameters for the window. We should do some of that here, especially color depth
    // (because it's interesting and can be fun to play with!)
   
    if (!glfwInit())
    {
        return std::unexpected(RhiError::GLFWInitFailed);
    }

    // works like Vulkan - no context, just platform window
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    GLFWwindow* window = glfwCreateWindow(createInfo.InitialWidth, createInfo.InitialHeight, createInfo.ApplicationName.data(), nullptr, nullptr);
    if (!window)
    {
        return std::unexpected(RhiError::GLFWWindowCreationFailed);
    }

    // incase we need this for callbacks later
    glfwSetWindowUserPointer(window, this);

    return window;
}

std::expected<wgpu::Surface, RhiError> Context::createSurface(const ContextCreateInfo& /*createInfo*/)
{
    // todo: this GLFW shim sets the descriptor based on GLFW hints, but for things like colorspaces this won't pass through
    // at least it didn't in DiamondDogs, not without a good bit of extra work
    surface = wgpu::glfw::CreateSurfaceForWindow(instance, nativeWindow);
    if (!surface)
    {
        return std::unexpected(RhiError::SurfaceCreationFailed);
    }
    return surface;
}

void Context::configureSurface(const ContextCreateInfo& createInfo)
{
    wgpu::SurfaceCapabilities capabilities{};
    surface.GetCapabilities(adapter, &capabilities);

    auto format_match =
        [&createInfo](wgpu::TextureFormat format)
        { 
            return format == createInfo.PreferredSurfaceFormat;
        };

    // print suported formats
    std::string supportedFormats;
    for (size_t i = 0; i < capabilities.formatCount; ++i)
    {
        supportedFormats += std::format(" {} |", magic_enum::enum_name(capabilities.formats[i]));
    }
    std::println(stderr, "[velox][context] Surface supported formats:{}", supportedFormats);

    auto format_iter = std::find_if(capabilities.formats,
                                    capabilities.formats + capabilities.formatCount,
                                    format_match);
    wgpu::TextureFormat surfaceFormat{};
    if (format_iter != capabilities.formats + capabilities.formatCount)
    {
        surfaceFormat = *format_iter;
        std::println(stderr, "[velox][context] Using preferred surface format {}", magic_enum::enum_name(surfaceFormat));
    }
    else
    {
        // if we can't find our preferred format, just pick the first one the surface supports
        surfaceFormat = capabilities.formats[0];
        std::println(stderr, "[velox][context] Preferred surface format {} not supported by surface, using {} instead",
                     magic_enum::enum_name(createInfo.PreferredSurfaceFormat),
                     magic_enum::enum_name(surfaceFormat));
    }

    surfaceConfig.device = device;
    surfaceConfig.format = surfaceFormat;
    surfaceConfig.usage = wgpu::TextureUsage::RenderAttachment;
    surfaceConfig.width = createInfo.InitialWidth;
    surfaceConfig.height = createInfo.InitialHeight;
    // todo: assess later how/if we may want to change alpha mode
    surfaceConfig.alphaMode = wgpu::CompositeAlphaMode::Auto;
    surfaceConfig.presentMode = createInfo.PreferredPresentationMode;
#ifdef __EMSCRIPTEN__
    // this is only valid on emscripten: on native, we don't need to tell the compositor
    // or surface what colorspace we're in or to respect our tonemapping
    wgpu::SurfaceColorManagement colorManagement{};
    colorManagement.colorSpace = createInfo.PreferredColorSpace;
    colorManagement.toneMappingMode = createInfo.PreferredToneMappingMode;
    surfaceConfig.nextInChain = &colorManagement;
#endif
    surface.Configure(&surfaceConfig);
}

} // namespace velox
