#pragma once
#ifndef VELOX_RHI_ASYNC_TASKS_HPP
#define VELOX_RHI_ASYNC_TASKS_HPP
#include "Task.hpp"
#include "VeloxErrors.hpp"
#include <print>
#include <span>
#include <variant>
#include <webgpu/webgpu_cpp.h>
#ifndef NDEBUG
#include <magic_enum/magic_enum.hpp>
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

namespace detail
{
    template<wgpu::MapMode Mode>
    using MappedPointerType = std::conditional_t<Mode == wgpu::MapMode::Read, const void*, void*>;

    inline void PrintStatusMessage(wgpu::MapAsyncStatus status, wgpu::StringView message)
    {
#ifndef NDEBUG
        std::string_view statusText = magic_enum::enum_name(status);
        std::string_view wgpuMessage(message.data, message.length);
        std::println(stderr,
                     "[velox][async] Buffer map unsuccessful | status {} | message {}",
                     statusText,
                     wgpuMessage);
#endif
    }

    // we could probably merge these by templating on status enum and adding a const char*
    // for the type (buffer map/pipeline create) of command it's doing
    inline void PrintStatusMessage(wgpu::CreatePipelineAsyncStatus status, wgpu::StringView message)
    {
#ifndef NDEBUG
        std::string_view statusText = magic_enum::enum_name(status);
        std::string_view wgpuMessage(message.data, message.length);
        std::println(stderr,
                     "[velox][async] Pipeline create unsuccesful | status {} | message {}",
                     statusText,
                     wgpuMessage);
#endif
    }

    using PipelineResult = std::variant<wgpu::RenderPipeline, wgpu::ComputePipeline>;
} // namespace detail

// All of our async work is enqueued similarly: a status code (which has the same size, so we can
// type-erase it) and a continuation handle. That's it!
struct AsyncWorkRequest
{
    AsyncWorkRequest() noexcept
    {
    }
    ~AsyncWorkRequest() noexcept
    {
    }
    AsyncWorkRequest(const AsyncWorkRequest& other) = delete;
    AsyncWorkRequest(AsyncWorkRequest&& other) noexcept
        : StatusCode{std::move(other.StatusCode)},
          Continuation{std::move(other.Continuation)},
          pipeline{std::move(other.pipeline)}
    {
    }
    AsyncWorkRequest& operator=(AsyncWorkRequest&& other) noexcept
    {
        if (this != &other)
        {
            StatusCode = std::move(other.StatusCode);
            Continuation = std::move(other.Continuation);
            pipeline = std::move(other.pipeline);
        }
        return *this;
    }

    // We'll static_cast to the right enum value where we need it
    uint32_t StatusCode{0u};
    std::coroutine_handle<> Continuation{nullptr};
    // Pipeline creation is a bit more complex and may need a pipeline handle to be stored
    detail::PipelineResult pipeline;
};


struct AdapterAwaitable
{
    wgpu::Instance instance;
    wgpu::RequestAdapterOptions options;
    std::expected<wgpu::Adapter, velox::RhiError> result;

    // always suspend
    constexpr bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle)
    {
        instance.RequestAdapter(
            &options,
            wgpu::CallbackMode::AllowSpontaneous,
            [this, handle](
                wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message)
            {
                if (status == wgpu::RequestAdapterStatus::Success)
                {
                    result = adapter;
                }
                else
                {
                    std::println(
                        stderr,
                        "[velox][context] RequestAdapter failed with status {} and message: {}",
                        magic_enum::enum_name(status),
                        std::string_view(message.data, message.length));
                    result = std::unexpected(velox::RhiError::AdapterRequestFailed);
                }
                handle.resume();
            });
    }

    std::expected<wgpu::Adapter, velox::RhiError> await_resume() noexcept
    {
        return std::move(result);
    }
};

struct DeviceAwaitable
{
    wgpu::Adapter adapter;
    wgpu::DeviceDescriptor descriptor;
    std::expected<wgpu::Device, velox::RhiError> result;

    constexpr bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle)
    {
        adapter.RequestDevice(
            &descriptor,
            wgpu::CallbackMode::AllowSpontaneous,
            [this, handle](
                wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message)
            {
                if (status == wgpu::RequestDeviceStatus::Success)
                {
                    result = device;
                }
                else
                {
                    std::println(
                        stderr,
                        "[velox][context] RequestDevice failed with status {} and message: {}",
                        magic_enum::enum_name(status),
                        std::string_view(message.data, message.length));
                    result = std::unexpected(velox::RhiError::DeviceRequestFailed);
                }
                handle.resume();
            });
    }

    std::expected<wgpu::Device, velox::RhiError> await_resume() noexcept
    {
        return std::move(result);
    }

    constexpr explicit operator bool() const noexcept
    {
        return result.has_value();
    }

    velox::RhiError error() const noexcept
    {
        return result.error();
    }
};


template<wgpu::MapMode Mode>
struct BufferMapAwaitable
{
private:
    // checking wgpu headers, this is just 8 bytes, so a reference doesn't save us anything
    wgpu::Buffer buffer{};
    size_t size{std::numeric_limits<size_t>::max()};
    size_t offset{0u};
    MappedPointerType<Mode> data{nullptr};

public:
    constexpr BufferMapAwaitable(wgpu::Buffer buffer, size_t size, size_t offset = 0u) noexcept
        : buffer(buffer),
          size(size),
          offset(offset)
    {
    }

    constexpr bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle)
    {
        // await suspend registers the callback, meaning we immediately return to the caller
        // webgpu will call this captured callback, at which point it resumes the coroutine
        buffer.MapAsync(Mode,
                        offset,
                        size,
                        wgpu::CallbackMode::AllowSpontaneous,
                        [handle](wgpu::MapAsyncStatus status, wgpu::StringView message)
                        {
                            if (status != wgpu::MapAsyncStatus::Success)
                            {
                                detail::PrintStatusMessage(status, message);
                            }
                            handle.resume();
                        });
    }

    MappedPointerType<Mode> await_resume() const noexcept
    {
        if constexpr (Mode == wgpu::MapMode::Read)
        {
            return buffer.GetConstMappedRange(offset, size);
        }
        else if constexpr (Mode == wgpu::MapMode::Write)
        {
            return buffer.GetMappedRange(offset, size);
        }
    }
};

template<wgpu::MapMode Mode>
struct BufferMapSession
{
public:
    BufferMapSession(const BufferMapSession&) = delete;
    BufferMapSession& operator=(const BufferMapSession&) = delete;
    ~BufferMapSession()
    {
        buffer.Unmap();
    }

    static Task<BufferMapSession<Mode>>
    CreateAsync(wgpu::Buffer buffer, size_t _size, size_t _offset)
    {
        detail::MappedPointerType<Mode> dataPtr =
            co_await BufferMapAwaitable<Mode>{buffer, _size, _offset};
        co_return BufferMapSession<Mode>(buffer, _size, _offset, dataPtr);
    }

    template<typename T>
    std::span<T> GetDataAs() noexcept
    {
        static_assert(std::is_standard_layout_v<T>,
                      "T must be standard layout to interpret mapped data as span of T");
        return std::span<T>(static_cast<std::add_pointer_t<T>>(data), size / sizeof(T));
    }

    template<typename T>
    std::span<const T> GetDataAs() const noexcept
    {
        static_assert(std::is_standard_layout_v<T>,
                      "T must be standard layout to interpret mapped data as span of T");
        return std::span<const T>(static_cast<std::add_pointer_t<const T>>(data), size / sizeof(T));
    }

    detail::MappedPointerType<Mode> GetDataPtr() noexcept
    {
        return data;
    }

    detail::MappedPointerType<Mode> GetDataPtr() const noexcept
    {
        return data;
    }

private:
    BufferMapSession(wgpu::Buffer _buffer,
                     size_t _size,
                     size_t _offset,
                     detail::MappedPointerType<Mode> _data) noexcept
        : buffer{_buffer},
          size{_size},
          offset{_offset},
          data{_data}
    {
    }
    wgpu::Buffer buffer;
    size_t size;
    size_t offset;
    detail::MappedPointerType<Mode> data;
};

template<typename T>
concept PipelineType =
    std::is_same_v<T, wgpu::RenderPipeline> || std::is_same_v<T, wgpu::ComputePipeline>;

template<PipelineType Pipeline>
struct PipelineAwaitable
{
private:
    wgpu::Device device{};
    // store right descriptor type based on template
    using DescriptorType = std::conditional_t<std::is_same_v<Pipeline, wgpu::RenderPipeline>,
                                              wgpu::RenderPipelineDescriptor,
                                              wgpu::ComputePipelineDescriptor>;
    DescriptorType descriptor{};
    Pipeline result{};

public:
    // try to forward the descriptor since it contains a whole boatload of members
    constexpr PipelineAwaitable(wgpu::Device _device, DescriptorType _descriptor) noexcept
        : device{_device},
          descriptor{std::forward<DescriptorType>(_descriptor)}
    {
    }

    ~PipelineAwaitable() noexcept
    {
    }

    constexpr bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle)
    {
        if constexpr (std::is_samve_v<Pipeline, wgpu::RenderPipeline>)
        {
            auto callback = [handle](wgpu::CreatePipelineAsyncStatus status,
                                     wgpu::RenderPipeline pipeline,
                                     wgpu::StringView message)
            {
                if (status != wgpu::CreatePipelineAsyncStatus::Success)
                {
                    detail::PrintStatusMessage(status, message);
                }
                handle.resume();
            };

            device.CreateRenderPipelineAsync(
                descriptor, wgpu::CallbackMode::AllowSpontaneous, callback);
        }
        else if constexpr (std::is_same_v<Pipeline, wgpu::ComputePipeline>)
        {
            device.CreateComputePipelineAsync(
                descriptor, wgpu::CallbackMode::AllowSpontaneous, callback);
        }
    }

    Pipeline await_resume() const noexcept
    {
        return result;
    }
};

} // namespace velox

#endif // !VELOX_RHI_ASYNC_TASKS_HPP
