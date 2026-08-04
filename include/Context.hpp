#pragma once

#ifndef VELOX_WEB_GPU_CONTEXT_HPP
#define VELOX_WEB_GPU_CONTEXT_HPP
#include "VeloxErrors.hpp"
#include <webgpu/webgpu_cpp.h>
#include <cstdint>
#include <string_view>
#include <span>

#ifndef __EMSCRIPTEN__
struct GLFWwindow;
#endif

namespace velox
{
    enum class ResizeStatus : uint8_t
    {
        Unchanged = 0,
        Resized = 1,
        Minimized = 2
    };

    struct ContextCreateInfo
    {
        uint32_t InitialWidth{ 800u };
        uint32_t InitialHeight{ 600u };
        std::string_view ApplicationName{ "WebGPU App" };
        // This is for *device* features only
        std::span<wgpu::FeatureName> RequiredFeatures;
        wgpu::FeatureLevel FeatureLevel{ wgpu::FeatureLevel::Core };
        wgpu::PowerPreference PowerPreference{ wgpu::PowerPreference::HighPerformance };

        // following are swapchain parameters: named "preferred" because we will try to use them,
        // but won't crash or fail if the surface doesn't support them (support varies a LOT ime)
        // Undefined => pick the first format the surface reports as supported
        wgpu::TextureFormat PreferredSurfaceFormat{ wgpu::TextureFormat::Undefined };
        // todo: how does HDR support actually work? we'll need a tonemapper too....
        wgpu::PredefinedColorSpace PreferredColorSpace{ wgpu::PredefinedColorSpace::SRGB };
        wgpu::PresentMode PreferredPresentationMode{ wgpu::PresentMode::Fifo };
    #ifdef __EMSCRIPTEN__
        std::string_view CanvasSelector{ "#canvas" };
    #endif
    };

    /**
     * @brief Owns the classical Instance/Adapter/Device trio, but also has full
     * ownership of the surface and queue. Also handles a few bookkeeping and
     * common callbacks like device losses, surface reconfig, etc. This class
     * mostly exists to centralize setup. 
     * 
     * Based on my RhiSystem implementation from DiamondDogs repo. Does not 
     * allow access to queues, resources, or command buffers: purely holds
     * the baseline objects we need to get a WebGPU context online.
    */
    class Context
    {
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
    public:

    #ifdef __EMSCRIPTEN__
        explicit Context(const ContextCreateInfo& createInfo);
    #else
        Context(const ContextCreateInfo& createInfo);
    #endif
        ~Context();

        ResizeStatus Resize(uint32_t width, uint32_t height);
        wgpu::TextureView AcquireNextFrame();
        void Present();

        const wgpu::Instance& GetInstance() const noexcept;
        const wgpu::Adapter& GetAdapter() const noexcept;
        const wgpu::Device& GetDevice() const noexcept;
        const wgpu::Queue& GetQueue() const noexcept;
        const wgpu::Surface& GetSurface() const noexcept;
        wgpu::TextureFormat GetSurfaceFormat() const noexcept;

        bool HasFeature(wgpu::FeatureName feature) const noexcept;

    private:

        std::expected<wgpu::Instance, RhiError> requestInstance(const ContextCreateInfo& createInfo);
        std::expected<wgpu::Adapter, RhiError> requestAdapter(const ContextCreateInfo& createInfo);
        std::expected<wgpu::Device, RhiError> requestDevice(const ContextCreateInfo& createInfo);
    #ifdef __EMSCRIPTEN__
        std::expected<wgpu::Surface, RhiError> createSurface(const ContextCreateInfo& createInfo);
    #else
        // need to make sure a valid GLFWwindow* is created before we create the surface
        std::expected<GLFWwindow*, RhiError> createNativeWindow(const ContextCreateInfo& createInfo);
        std::expected<wgpu::Surface, RhiError> createSurface(const ContextCreateInfo& createInfo);
    #endif

        void configureSurface(const ContextCreateInfo& createInfo);

        wgpu::Instance instance;
        wgpu::Adapter adapter;
        wgpu::Device device;
        wgpu::Queue queue;
#ifndef __EMSCRIPTEN__
        GLFWwindow* nativeWindow{ nullptr };
#endif
        wgpu::Surface surface;
        wgpu::TextureFormat surfaceFormat{ wgpu::TextureFormat::Undefined };
        // we store the surface config to make reconfiguring not need the whole create info
        wgpu::SurfaceConfiguration surfaceConfig{};
    };

} // namespace velox

#endif // !VELOX_WEB_GPU_CONTEXT_HPP
