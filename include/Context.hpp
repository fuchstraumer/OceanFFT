#pragma once

#ifndef VELOX_WEB_GPU_CONTEXT_HPP
#define VELOX_WEB_GPU_CONTEXT_HPP
#include "utility/SlotMap.hpp"
#include "VeloxErrors.hpp"
#include <cstdint>
#include <span>
#include <coroutine>
#include <expected>
#include <webgpu/webgpu_cpp.h>

struct GLFWwindow;

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
    wgpu::ToneMappingMode PreferredToneMappingMode{ wgpu::ToneMappingMode::Standard };
    wgpu::PresentMode PreferredPresentationMode{ wgpu::PresentMode::Fifo };
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
    Context(const ContextCreateInfo& createInfo);
    ~Context();

    // use coroutines to do async work, so we can avoid asyncify
    // we can't use coroutines in a ctor, so it has to be a separate function!
    //Task<std::expected<bool, RhiError>> InitWebGPU(const ContextCreateInfo& createInfo);

    ResizeStatus Resize(uint32_t width, uint32_t height);
    wgpu::TextureView AcquireNextFrame();
    void Present();

    wgpu::Instance& GetInstance() noexcept;
    wgpu::Adapter& GetAdapter() noexcept;
    wgpu::Device& GetDevice() noexcept;
    wgpu::Queue& GetQueue() noexcept;
    wgpu::Surface& GetSurface() noexcept;
    wgpu::TextureFormat GetSurfaceFormat() const noexcept;

    bool HasFeature(wgpu::FeatureName feature) const noexcept;
    GLFWwindow* GetNativeWindow() const noexcept;

    SlotMapHandle RegisterPending(std::coroutine_handle<> deferred_coroutine) noexcept;
    void MarkReady(SlotMapHandle handle) noexcept;

private:
    std::expected<wgpu::Instance, RhiError> requestInstance(const ContextCreateInfo& createInfo);
    wgpu::RequestAdapterOptions getAdapterOptions(const ContextCreateInfo& createInfo) const;
    std::expected<wgpu::Adapter, RhiError> requestAdapter(const ContextCreateInfo& createInfo);
    wgpu::DeviceDescriptor getDeviceDescriptor(const ContextCreateInfo& createInfo) const;
    std::expected<wgpu::Device, RhiError> requestDevice(const ContextCreateInfo& createInfo);
    std::expected<GLFWwindow*, RhiError> createNativeWindow(const ContextCreateInfo& createInfo);
    std::expected<wgpu::Surface, RhiError> createSurface(const ContextCreateInfo& createInfo);

    void configureSurface(const ContextCreateInfo& createInfo);

    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Queue queue;
    GLFWwindow* nativeWindow{ nullptr };

    wgpu::Surface surface;
    // we store the surface config to make reconfiguring not need the whole create info
    wgpu::SurfaceConfiguration surfaceConfig{};

};

} // namespace velox

#endif // !VELOX_WEB_GPU_CONTEXT_HPP
