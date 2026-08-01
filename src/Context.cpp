#include "Context.hpp"
#include <format>
#include <iostream>
#include <print>
#ifndef __EMSCRIPTEN__
#include <GLFW/glfw3.h>
#include <webgpu/webgpu_glfw.h>
#endif
#include <unordered_map>
#include <algorithm>

namespace
{
    static const std::unordered_map<wgpu::ErrorType, std::string_view> ErrorTypeStrings
    {
        { wgpu::ErrorType::NoError, "No Error" },
        { wgpu::ErrorType::Validation, "Validation" },
        { wgpu::ErrorType::OutOfMemory, "Out of Memory" },
        { wgpu::ErrorType::Internal, "Internal" },
        { wgpu::ErrorType::Unknown, "Unknown" }
    };

    static const std::unordered_map<wgpu::DeviceLostReason, std::string_view> DeviceLostReasonStrings
    {
        { wgpu::DeviceLostReason::Unknown, "Unknown" },
        { wgpu::DeviceLostReason::Destroyed, "Destroyed" },
        { wgpu::DeviceLostReason::CallbackCancelled, "Callback Cancelled" },
        { wgpu::DeviceLostReason::FailedCreation, "Creation Failed" }
    };

    // todo: we should sink these somewhere more portable, and which could actually give us debug info in live clients maybe?
    void LogUncapturedError(const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message)
    {
        const auto iter = ErrorTypeStrings.find(type);
        const std::string_view typeStr = (iter != ErrorTypeStrings.end()) ? iter->second : "Unknown";
        std::println(stderr,
                     "[wgpu] Uncaptured error | Type \"{}\" | Message: {}",
                     typeStr, std::string_view(message.data, message.length));
    }

    void LogDeviceLost(const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message)
    {
        // note that this is also called for routine destruction, so messages from here don't always mean something went wrong
        const auto it = DeviceLostReasonStrings.find(reason);
        const std::string_view reasonStr = (it != DeviceLostReasonStrings.end()) ? it->second : "Unknown";
        std::println(stderr,
                     "[wgpu] Device lost | Reason: \"{}\" | Message: {}",
                     reasonStr, std::string_view(message.data, message.length));
    }
}

namespace velox
{

#ifdef __EMSCRIPTEN__
Context::Context(const ContextCreateInfo& createInfo)
#else
Context::Context(const ContextCreateInfo& createInfo, GLFWwindow* nativeWindow)
#endif
{
    wgpu::InstanceDescriptor instanceDesc{};
#ifndef __EMSCRIPTEN__
    const wgpu::InstanceFeatureName requiredFeatures[] = { wgpu::InstanceFeatureName::TimedWaitAny };
    instanceDesc.requiredFeatureCount = std::size(requiredFeatures);
    instanceDesc.requiredFeatures = requiredFeatures;
    instance = ValidOrExit(requestInstance(createInfo));
#endif

    adapter = ValidOrExit(requestAdapter(createInfo));
    device = ValidOrExit(requestDevice(createInfo));
    queue = device.GetQueue();

#ifdef __EMSCRIPTEN__
    surface = ValidOrExit(createSurface(createInfo));
#else
    // createnativewindow() also initializes glfw, mostly just to keep code tidy
    nativeWindow = ValidOrExit(createNativeWindow(createInfo));
    surface = ValidOrExit(createSurface(createInfo, nativeWindow));
#endif
}

Context::~Context()
{
    glfwDestroyWindow(nativeWindow);
    glfwTerminate();
}

std::expected<wgpu::Instance, RhiError> Context::requestInstance(const ContextCreateInfo& createInfo)
{
    wgpu::InstanceDescriptor instanceDesc{};
    instanceDesc.nextInChain = nullptr;
    wgpu::Instance instance = wgpu::CreateInstance(&instanceDesc);
    if (!instance)
    {
        return std::unexpected(RhiError::AdapterRequestFailed);
    }
    return instance;
}

std::expected<wgpu::Adapter, RhiError> Context::requestAdapter(const ContextCreateInfo& createInfo)
{
    wgpu::RequestAdapterOptions options{};
#ifndef __EMSCRIPTEN__
    options.backendType = wgpu::BackendType::Vulkan; // default to vulkan, since we know it best
#endif
    options.featureLevel = createInfo.FeatureLevel;
    options.powerPreference = createInfo.PowerPreference;
    // todo: compatible surface parameter. unused right now, but I think it could let us query for HDR backbuffer support?
    // diamonddogs does this through glfw and win32 api calls, but I think we can do it through wgpu directly if we pass the surface here.

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
                std::println(stderr, "RequestAdapter failed: {}", std::string_view(message.data, message.length));
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
    deviceDesc.requiredFeatureCount = createInfo.RequiredFeatures.size();
    deviceDesc.requiredFeatures = createInfo.RequiredFeatures.data();
    deviceDesc.SetUncapturedErrorCallback(LogUncapturedError);
    deviceDesc.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous, LogDeviceLost);
    // todo: leaving required limits blank clamps to minspec, which is fine for now. will have to assess this better in future
    //deviceDesc.requiredLimits = nullptr;
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
                std::println(stderr, "RequestDevice failed: {}", std::string_view(message.data, message.length));
            }
        });

    instance.WaitAny(future, UINT64_MAX);

    if (!result_device)
    {
        return std::unexpected(RhiError::DeviceRequestFailed);
    }

    return result_device;
}

#ifdef __EMSCRIPTEN__
std::expected<wgpu::Surface, RhiError> Context::createSurface(const ContextCreateInfo& createInfo)
{
    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc{};
    canvasDesc.selector = createInfo.CanvasSelector.c_str();
    wgpu::SurfaceColorManagement colorDesc{};
    colorDesc.colorSpace = createInfo.PreferredColorSpace;
    // todo: what is Extended tonemapping? do we not have the ability to run our own? is this mobile weirdness?
    colorDesc.toneMappingMode = wgpu::ToneMappingMode::Standard;
    // make sure chaining is set right
    canvasDesc.nextInChain = &colorDesc;
    colorDesc.nextInChain = nullptr;

    wgpu::SurfaceDescriptor surfaceDesc{};
    surfaceDesc.nextInChain = &canvasDesc;
    surface = instance.CreateSurface(&surfaceDesc);
    if (!surface)
    {
        return std::unexpected(RhiError::SurfaceConfigurationFailed);
    }
    return surface;
}
#else
std::expected<GLFWwindow*, RhiError> Context::createNativeWindow(const ContextCreateInfo& createInfo)
{
    if (!glfwInit())
    {
        return std::unexpected(RhiError::GLFWInitFailed);
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // don't create an OpenGL context
    GLFWwindow* window = glfwCreateWindow(createInfo.InitialWidth, createInfo.InitialHeight, createInfo.ApplicationName.data(), nullptr, nullptr);
    if (!window)
    {
        return std::unexpected(RhiError::GLFWWindowCreationFailed);
    }

    return window;
}


std::expected<wgpu::Surface, RhiError> Context::createSurface(const ContextCreateInfo& /*createInfo*/, GLFWwindow* nativeWindow)
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
#endif

void Context::configureSurface(const ContextCreateInfo& createInfo)
{
    wgpu::SurfaceCapabilities capabilities{};
    surface.GetCapabilities(adapter, &capabilities);

    auto format_match =
        [&createInfo](wgpu::TextureFormat format)
        { 
            return format == createInfo.PreferredSurfaceFormat;
        };
    // traverse array of caps and use std::find_if to find the first format
    // that matches our preferred format. then we go on to guessing. heuristically. :)
    auto format_iter = std::find_if(capabilities.formats,
                                    capabilities.formats + capabilities.formatCount,
                                    format_match);

    surfaceConfig.device = device;
    surfaceConfig.format = surfaceFormat;
    surfaceConfig.usage = wgpu::TextureUsage::RenderAttachment;
    surfaceConfig.width = createInfo.InitialWidth;
    surfaceConfig.height = createInfo.InitialHeight;
    // todo: assess later how/if we may want to change alpha mode
    surfaceConfig.alphaMode = wgpu::CompositeAlphaMode::Auto;
    surfaceConfig.presentMode = createInfo.PreferredPresentationMode;
    surface.Configure(&surfaceConfig);

}

void Context::Resize(uint32_t width, uint32_t height)
{
    if (width == 0u || height == 0u)
    {
        return; // minimized / zero-size - skip until it's real again
    }
    // just update dims, and then reconfigure
    surfaceConfig.width = width;
    surfaceConfig.height = height;
    surface.Configure(&surfaceConfig);
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
        std::println(stderr, "[velox][wgpu] Next frame acquisition returned SuccessSuboptimal");
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

bool Context::HasFeature(wgpu::FeatureName feature) const noexcept
{
    return device.HasFeature(feature);
}

} // namespace velox