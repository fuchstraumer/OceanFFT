#pragma once
#ifndef WEB_GPU_CONTEXT_HPP
#define WEB_GPU_CONTEXT_HPP
#include <webgpu/webgpu_cpp.h>
#include <cstdint>
#include <string_view>

#ifndef __EMSCRIPTEN__
struct GLFWwindow;
#endif

struct ContextCreateInfo
{
    std::string_view ApplicationName{ "WebGPU App" };
    std::vector<wgpu::FeatureName> RequiredFeatures;
    // Undefined => pick the first format the surface reports as supported
    wgpu::TextureFormat PreferredSurfaceFormat{ wgpu::TextureFormat::Undefined };
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
 * Based on my RhiSystem implementation from DiamondDogs repo
*/
class Context
{
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
public:

#ifdef __EMSCRIPTEN__
    explicit Context(const ContextCreateInfo& createInfo);
#else
    Context(const ContextCreateInfo& createInfo, GLFWwindow* nativeWindow);
#endif
    ~Context();

    void Resize(uint32_t width, uint32_t height);
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

    void requestAdapter(const ContextCreateInfo& createInfo);
    void requestDevice(const ContextCreateInfo& createInfo);
#ifdef __EMSCRIPTEN__
    void createSurface(const ContextCreateInfo& createInfo);
#else
    void createSurface(const ContextCreateInfo& createInfo, GLFWwindow* nativeWindow);
#endif

    void configureSurface(uint32_t width, uint32_t height);

    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Queue queue;
    wgpu::Surface surface;
    wgpu::TextureFormat surfaceFormat{ wgpu::TextureFormat::Undefined };
    uint32_t surfaceWidth{ 0u };
    uint32_t surfaceHeight{ 0u };
};

#endif //!WEB_GPU_CONTEXT_HPP
