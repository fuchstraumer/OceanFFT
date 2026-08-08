#pragma once
#ifndef VELOX_RHI_ASYNC_TASKS_HPP
#define VELOX_RHI_ASYNC_TASKS_HPP
#include "utility/SlotMap.hpp"
#include "VeloxErrors.hpp"
#include <coroutine>
#include <expected>
#include <memory>
#include <webgpu/webgpu_cpp.h>

#ifndef NDEBUG
#include <magic_enum/magic_enum.hpp>
#include <print>
#endif

/**
 * @brief Various helper functions for scheduling and marshalling async tasks for WebGPU, namely
 * mapping and pipeline creation. These are all implemented as coroutines, with most of the tasks
 * being put into a slotmap we use as a global queue for async work - with continuations used to
 * return to the caller once the async work is complete. This works nicely, and allows us to write
 * more of our code in a synchronous style, while still being async under the hood.
 *
 * I also use Session() structs as RAII wrappers to manage lifetimes of things like
 * mapping/unmapping
 */
namespace velox
{

struct Scheduler;

namespace detail
{

    // todo: for operations that have more than just success/fail status enum values, we need to
    // have Velox error enum values that match those to store in std::unexpected
    template<typename StatusEnum>
    inline void PrintStatusMessage(const char* ourMsg, StatusEnum status, wgpu::StringView message)
    {
#ifndef NDEBUG
        std::string_view statusText = magic_enum::enum_name(status);
        std::string_view wgpuMessage(message.data, message.length);
        std::println(stderr, "[velox][async] {} | status: {} | message: {}", ourMsg, statusText, wgpuMessage);
#endif
    }

} // namespace detail

template<typename T>
using Result = std::expected<T, RhiError>;

struct AdapterAwaitable
{
    wgpu::Instance instance;
    wgpu::RequestAdapterOptions options;
    Result<wgpu::Adapter> result;

    // always suspend
    constexpr bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle);

    Result<wgpu::Adapter> await_resume() noexcept;

    constexpr explicit operator bool() const noexcept
    {
        return result.has_value();
    }
};

struct DeviceAwaitable
{
    wgpu::Adapter adapter;
    wgpu::DeviceDescriptor descriptor;
    Result<wgpu::Device> result;

    constexpr bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle);

    Result<wgpu::Device> await_resume() noexcept;

    constexpr explicit operator bool() const noexcept
    {
        return result.has_value();
    }
};

struct MapReadAwaitable
{
private:
    wgpu::Buffer buffer{};
    size_t size{ std::numeric_limits<size_t>::max() };
    size_t offset{ 0u };
    Result<const void*> result;
    Scheduler* scheduler{ nullptr };

public:
    MapReadAwaitable(wgpu::Buffer _buffer,
                     size_t _size,
                     size_t _offset = 0u,
                     Scheduler* _scheduler = nullptr) noexcept;

    constexpr bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle);

    Result<const void*> await_resume() const noexcept;
};

struct MapWriteAwaitable
{
private:
    wgpu::Buffer buffer{};
    size_t size{ std::numeric_limits<size_t>::max() };
    size_t offset{ 0u };
    Result<void*> result;
    Scheduler* scheduler{ nullptr };

public:
    MapWriteAwaitable(wgpu::Buffer _buffer,
                      size_t _size,
                      size_t _offset = 0u,
                      Scheduler* _scheduler = nullptr) noexcept;

    constexpr bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle);

    Result<void*> await_resume() const noexcept;
};

template<typename MapType>
struct MapSession
{
    using PointerType = std::conditional_t<std::is_same_v<MapReadAwaitable, MapType>, const void*, void*>;
public:
    MapSession(const MapSession&) = delete;
    MapSession& operator=(const MapSession&) = delete;
    ~MapSession()
    {
        buffer.Unmap();
    }

    // static Task<MapSession<Mode>> CreateAsync(wgpu::Buffer buffer, size_t _size, size_t _offset)
    //{
    //     MapResultType<Mode> mapResult = co_await BufferMapAwaitable<Mode>{ buffer, _size, _offset };
    //     co_return MapSession<Mode>(buffer, _size, _offset, data);
    // }

    template<typename T>
        requires(std::is_same_v<MapType, MapWriteAwaitable>)
    std::span<T> GetDataAs() noexcept
    {
        assert(reinterpret_cast<std::uintptr_t>(mappedPtr) % alignof(T) == 0);
        static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>,
                      "T must be standard layout to interpret mapped data as span of T");
        const size_t numElements = size / sizeof(T);
        T* typeArray = std::start_lifetime_as_array<T>(mappedPtr, numElements);
        return std::span<T>(typeArray, typeArray + numElements);
    }

    template<typename T>
        requires(std::is_same_v<MapType, MapReadAwaitable>)
    std::span<const T> GetDataAs() const noexcept
    {
        assert(reinterpret_cast<std::uintptr_t>(mappedPtr) % alignof(T) == 0);
        static_assert(
            std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>,
            "T must be standard layout and trivially copyable to interpret mapped data as span of T");
        const size_t numElements = size / sizeof(T);
        const T* typeArray = std::start_lifetime_as_array<T>(mappedPtr, numElements);
        return std::span<const T>(typeArray, typeArray + numElements);
    }

    void* GetDataPtr() noexcept requires(std::is_same_v<MapType, MapWriteAwaitable>)
    {
        return mappedPtr;
    }

    const void* GetDataPtr() const noexcept requires(std::is_same_v<MapType, MapReadAwaitable>)
    {
        return mappedPtr;
    }

private:
    MapSession(wgpu::Buffer _buffer, size_t _size, size_t _offset, PointerType _data) noexcept
        : buffer{ _buffer },
          size{ _size },
          offset{ _offset },
          mappedPtr{ _data }
    {
    }
    wgpu::Buffer buffer;
    size_t size;
    size_t offset;
    PointerType mappedPtr;
};

struct RenderPipelineAwaitable
{
private:
    wgpu::Device device{};
    wgpu::RenderPipelineDescriptor descriptor{};
    Result<wgpu::RenderPipeline> result{};
    Scheduler* scheduler{ nullptr };

public:
    RenderPipelineAwaitable(wgpu::Device _device,
                            wgpu::RenderPipelineDescriptor _descriptor,
                            Scheduler* _scheduler) noexcept;
    ~RenderPipelineAwaitable() noexcept;
    RenderPipelineAwaitable(const RenderPipelineAwaitable&) = delete;
    RenderPipelineAwaitable& operator=(const RenderPipelineAwaitable&) = delete;
    
    // todo: in the future, we could probably query a cache here and immediate return already built pipelines!
    // alternatively: hook them up to the same coroutine slot, so they're alerted on wakeup too?
    constexpr bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle);
    Result<wgpu::RenderPipeline> await_resume() noexcept;

};

struct ComputePipelineAwaitable
{
private:
    wgpu::Device device{};
    wgpu::ComputePipelineDescriptor descriptor{};
    Result<wgpu::ComputePipeline> result{};
    Scheduler* scheduler{ nullptr };
public:
    ComputePipelineAwaitable(wgpu::Device _device,
                             wgpu::ComputePipelineDescriptor _descriptor,
                             Scheduler* _scheduler) noexcept;
    ~ComputePipelineAwaitable() noexcept;
    ComputePipelineAwaitable(const ComputePipelineAwaitable&) = delete;
    ComputePipelineAwaitable& operator=(const ComputePipelineAwaitable&) = delete;

    constexpr bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle);
    Result<wgpu::ComputePipeline> await_resume() noexcept;
};

} // namespace velox

#endif // !VELOX_RHI_ASYNC_TASKS_HPP
