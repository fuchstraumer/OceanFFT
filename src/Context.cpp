#include "Context.hpp"
#include <format>
#include <iostream>
#ifndef __EMSCRIPTEN__
#include <GLFW/glfw3.h>
#include <webgpu/webgpu_glfw.h>
#endif

namespace
{   
    // todo: we should sink these somewhere more portable, and which could actually give us debug info in live clients maybe?
    void LogUncapturedError(const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message)
    {
        std::cerr << std::format("[wgpu] Uncaptured error ({}): {}\n",
            static_cast<int>(type), std::string_view(message.data, message.length));
    }

    void LogDeviceLost(const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message)
    {
        std::cerr << std::format("[wgpu] Device lost ({}): {}\n",
            static_cast<int>(reason), std::string_view(message.data, message.length));
    }
}

#ifdef __EMSCRIPTEN__
Context::Context(const ContextCreateInfo& createInfo)
#else
Context::Context(const ContextCreateInfo& createInfo, GLFWwindow* nativeWindow)
#endif
{
    wgpu::InstanceDescriptor instanceDesc{};
}
