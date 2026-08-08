#pragma once
#ifndef VELOX_RHI_ASYNC_TASKS_HPP
#define VELOX_RHI_ASYNC_TASKS_HPP
#include "Context.hpp"
#include <coroutine>

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

template<wgpu::MapMode Mode>
using MappedPointerType = std::conditional_t<Mode == wgpu::MapMode::Read, const void*, void*>;

template<wgpu::MapMode Mode>
using MapResultType = Result<MappedPointerType<Mode>>;

template<typename T>
concept PipelineType = std::is_same_v<T, wgpu::RenderPipeline> || std::is_same_v<T, wgpu::ComputePipeline>;

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

template<wgpu::MapMode Mode>
struct BufferMapAwaitable
{
private:
    // checking wgpu headers, this is just 8 bytes, so a reference doesn't save us anything
    wgpu::Buffer buffer{};
    size_t size{ std::numeric_limits<size_t>::max() };
    size_t offset{ 0u };
    MapResultType<Mode> result;
    Context* context{ nullptr };

public:
    constexpr BufferMapAwaitable(wgpu::Buffer _buffer,
                                 size_t _size,
                                 size_t _offset = 0u,
                                 Context* _context = nullptr) noexcept
        : buffer{ _buffer },
          size{ _size },
          offset{ _offset },
          context{ _context }
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

        // this is a deferred resume coroutine, register with context
        SlotMapHandle slot;
        if (context)
        {
            slot = context->RegisterPending(handle.address());
        }

        buffer.MapAsync(Mode,
                        offset,
                        size,
                        wgpu::CallbackMode::AllowSpontaneous,
                        [handle, slot, this](wgpu::MapAsyncStatus status, wgpu::StringView message)
                        {
                            if (status != wgpu::MapAsyncStatus::Success)
                            {
                                detail::PrintStatusMessage("MapBuffer", status, message);
                                result = std::unexpected(RhiError::MapAsyncFailed);
                            }

                            if (!context)
                            {
                                handle.resume();
                            }
                            else
                            {
                                context->MarkReady(slot);
                            }
                        });
    }

    MapResultType<Mode> await_resume() const noexcept
    {
        if (!result.has_value())
        {
            return result;
        }
        else [[likely]]
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

    // static Task<BufferMapSession<Mode>> CreateAsync(wgpu::Buffer buffer, size_t _size, size_t _offset)
    //{
    //     MapResultType<Mode> mapResult = co_await BufferMapAwaitable<Mode>{ buffer, _size, _offset };
    //     co_return BufferMapSession<Mode>(buffer, _size, _offset, data);
    // }

    template<typename T>
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
    std::span<const T> GetDataAs() const noexcept
    {
        assert(reinterpret_cast<std::uintptr_t>(mappedPtr) % alignof(T) == 0);
        static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>,
                      "T must be standard layout and trivially copyable to interpret mapped data as span of T");
        const size_t numElements = size / sizeof(T);
        const T* typeArray = std::start_lifetime_as_array<T>(mappedPtr, numElements);
        return std::span<const T>(typeArray, typeArray + numElements);
    }

    MappedPointerType<Mode> GetDataPtr() noexcept
    {
        return data;
    }

    MappedPointerType<Mode> GetDataPtr() const noexcept
    {
        return data;
    }

private:
    BufferMapSession(wgpu::Buffer _buffer,
                     size_t _size,
                     size_t _offset,
                     MappedPointerType<Mode> _data) noexcept
        : buffer{ _buffer },
          size{ _size },
          offset{ _offset },
          data{ _data }
    {
    }
    wgpu::Buffer buffer;
    size_t size;
    size_t offset;
    MappedPointerType<Mode> data;
};

template<PipelineType Pipeline>
struct PipelineAwaitable
{
private:
    wgpu::Device device{};
    using DescriptorType = std::conditional_t<std::is_same_v<Pipeline, wgpu::RenderPipeline>,
                                              wgpu::RenderPipelineDescriptor,
                                              wgpu::ComputePipelineDescriptor>;
    DescriptorType descriptor{};
    Result<Pipeline> result{};
    Context* context{ nullptr };

public:
    // try to forward the descriptor since it contains a whole boatload of members
    constexpr PipelineAwaitable(wgpu::Device _device, DescriptorType _descriptor, Context* _context) noexcept
        : device{ _device },
          descriptor{ std::forward<DescriptorType>(_descriptor) },
          context{ _context }
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
        SlotMapHandle slot;
        if (context)
        {
            slot = context->RegisterPending(handle.address());
        }

        if constexpr (std::is_same_v<Pipeline, wgpu::RenderPipeline>)
        {
            auto callback = [handle, slot, this](wgpu::CreatePipelineAsyncStatus status,
                                                 wgpu::RenderPipeline pipeline,
                                                 wgpu::StringView message)
            {
                if (status != wgpu::CreatePipelineAsyncStatus::Success)
                {
                    detail::PrintStatusMessage("CreateRenderPipelineAsync", status, message);
                    result = std::unexpected(RhiError::PipelineCreationFailed);
                }
                else [[likely]]
                {
                    result = pipeline;
                }

                if (context)
                {
                    context->MarkReady(slot);
                }
                else
                {
                    handle.resume();
                }
            };

            device.CreateRenderPipelineAsync(descriptor, wgpu::CallbackMode::AllowSpontaneous, callback);
        }
        else if constexpr (std::is_same_v<Pipeline, wgpu::ComputePipeline>)
        {
            auto callback = [handle, this](wgpu::CreatePipelineAsyncStatus status,
                                           wgpu::ComputePipeline pipeline,
                                           wgpu::StringView message)
            {
                if (status != wgpu::CreatePipelineAsyncStatus::Success)
                {
                    detail::PrintStatusMessage("CreateComputePiplineAsync", status, message);
                    result = std::unexpected(RhiError::PipelineCreationFailed);
                }
                else [[likely]]
                {
                    result = pipeline;
                }

                if (context)
                {
                    context->MarkReady(slot);
                }
                else
                {
                    handle.resume();
                }
            };
            device.CreateComputePipelineAsync(descriptor, wgpu::CallbackMode::AllowSpontaneous, callback);
        }
    }

    Result<Pipeline> await_resume() const noexcept
    {
        return result;
    }
};

// AsyncSlotType is what we store in our actual slotmap object, created when webgpu fires the callback
// from the event processing loop. This is typed for each result type we have, and stores a result
// and a continuation handle that we selectively resume ourselves. We need to be able to resume
// ourselves to make sure we're in a state ready to resume and work, not just being bombarded with
// surprise coroutine resumptions
template<typename T>
struct AsyncSlotType
{
    struct promise_type
    {
        T result_value;
        // map we will publish back into
        SlotMap<T, 512>* map;

        AsyncSlotType<T> get_return_object() noexcept
        {
            return AsyncSlotType<T>{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        // don't suspend initially, because we call the async function and let the awaitable
        // that constructs be what first sends us into suspension (after enqueuing our action)
        constexpr std::suspend_never initial_suspend() noexcept
        {
            // only initial step we perform: set result to nullopt, so try-get fails
            // until we actually fill it with a value on op complete
            result_value = std::nullopt;
            return {};
        }

        template<typename Awaitable>
        Awaitable&& await_transform(Awaitable&& await_inst) noexcept
        {
            static_assert("Didn't define appropriate await_transform for current type.");
        }

        // always suspend at the end so we can extract the result before destruction
        constexpr std::suspend_always final_suspend() noexcept
        {
            return {};
        }

        template<typename U>
        void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)
        {
            result_value = std::forward<U>(value);
        }

        void unhandled_exception()
        {
#ifndef __EMSCRIPTEN__
            std::terminate();
#else
            emscripten_force_exit(1);
#endif
        }
    };

    std::coroutine_handle<promise_type> handle;

    explicit AsyncSlotType(std::coroutine_handle<promise_type> _handle) noexcept
        : handle{ _handle }
    {
    }

    AsyncSlotType(const AsyncSlotType&) = delete;
    AsyncSlotType& operator=(const AsyncSlotType&) = delete;

    AsyncSlotType(AsyncSlotType&& other) noexcept = default;
    AsyncSlotType& operator=(AsyncSlotType&& other) = default;

    ~AsyncSlotType() = default;

    std::optional<T> TryGet()
    {
        if (!handle.done())
        {
            return std::nullopt;
        }
        else
        {
            return handle.promise().result_value;
        }
    }
};

} // namespace velox

#endif // !VELOX_RHI_ASYNC_TASKS_HPP
