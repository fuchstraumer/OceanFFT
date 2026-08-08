#include "AsyncTasks.hpp"
#include "Scheduler.hpp"
#include "utility/SlotMap.hpp"

namespace velox
{

void AdapterAwaitable::await_suspend(std::coroutine_handle<> handle)
{
    instance.RequestAdapter(
        &options,
        wgpu::CallbackMode::AllowSpontaneous,
        [this, handle](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message)
        {
            if (status != wgpu::RequestAdapterStatus::Success)
            {
                detail::PrintStatusMessage("RequestAdapter", status, message);
                result = std::unexpected(velox::RhiError::AdapterRequestFailed);
            }
            else [[likely]]
            {
                result = adapter;
            }
            handle.resume();
        });
}

Result<wgpu::Adapter> AdapterAwaitable::await_resume() noexcept
{
    return std::move(result);
}

void DeviceAwaitable::await_suspend(std::coroutine_handle<> handle)
{
    adapter.RequestDevice(
        &descriptor,
        wgpu::CallbackMode::AllowSpontaneous,
        [this, handle](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message)
        {
            if (status != wgpu::RequestDeviceStatus::Success)
            {
                detail::PrintStatusMessage("RequestDevice", status, message);
                result = std::unexpected(velox::RhiError::DeviceRequestFailed);
            }
            else [[likely]]
            {
                result = device;
            }
            handle.resume();
        });
}

Result<wgpu::Device> DeviceAwaitable::await_resume() noexcept
{
    return std::move(result);
}

MapReadAwaitable::MapReadAwaitable(wgpu::Buffer _buffer,
                                   size_t _size,
                                   size_t _offset,
                                   Scheduler* _scheduler) noexcept
    : buffer{ _buffer },
      size{ _size },
      offset{ _offset },
      scheduler{ _scheduler }
{
}

void MapReadAwaitable::await_suspend(std::coroutine_handle<> handle)
{
    // await suspend registers the callback, meaning we immediately return to the caller
    // webgpu will call this captured callback, at which point it resumes the coroutine

    // this is a deferred resume coroutine, register with context
    SlotMapHandle slot;
    if (scheduler)
    {
        slot = scheduler->RegisterPending(handle.address());
    }

    buffer.MapAsync(wgpu::MapMode::Read,
                    offset,
                    size,
                    wgpu::CallbackMode::AllowSpontaneous,
                    [handle, &slot, this](wgpu::MapAsyncStatus status, wgpu::StringView message)
                    {
                        if (status != wgpu::MapAsyncStatus::Success)
                        {
                            detail::PrintStatusMessage("MapBuffer", status, message);
                            result = std::unexpected(RhiError::MapAsyncFailed);
                        }

                        if (!scheduler)
                        {
                            handle.resume();
                        }
                        else
                        {
                            scheduler->MarkReady(std::move(slot));
                        }
                    });
}

Result<const void*> MapReadAwaitable::await_resume() const noexcept
{
    if (!result.has_value())
    {
        return result;
    }
    else [[likely]]
    {
        return buffer.GetConstMappedRange(offset, size);
    }
}

MapWriteAwaitable::MapWriteAwaitable(wgpu::Buffer _buffer,
                                     size_t _size,
                                     size_t _offset,
                                     Scheduler* _scheduler) noexcept
    : buffer{ _buffer },
      size{ _size },
      offset{ _offset },
      scheduler{ _scheduler }
{
}

void MapWriteAwaitable::await_suspend(std::coroutine_handle<> handle)
{
    // await suspend registers the callback, meaning we immediately return to the caller
    // webgpu will call this captured callback, at which point it resumes the coroutine

    // this is a deferred resume coroutine, register with context
    SlotMapHandle slot;
    if (scheduler)
    {
        slot = scheduler->RegisterPending(handle.address());
    }

    buffer.MapAsync(wgpu::MapMode::Write,
                    offset,
                    size,
                    wgpu::CallbackMode::AllowSpontaneous,
                    [handle, &slot, this](wgpu::MapAsyncStatus status, wgpu::StringView message)
                    {
                        if (status != wgpu::MapAsyncStatus::Success)
                        {
                            detail::PrintStatusMessage("MapBuffer", status, message);
                            result = std::unexpected(RhiError::MapAsyncFailed);
                        }

                        if (!scheduler)
                        {
                            handle.resume();
                        }
                        else
                        {
                            scheduler->MarkReady(std::move(slot));
                        }
                    });
}

Result<void*> MapWriteAwaitable::await_resume() const noexcept
{
    if (!result.has_value())
    {
        return result;
    }
    else [[likely]]
    {
        return buffer.GetMappedRange(offset, size);
    }
}

RenderPipelineAwaitable::RenderPipelineAwaitable(wgpu::Device _device,
                                                 wgpu::RenderPipelineDescriptor _descriptor,
                                                 Scheduler* _scheduler) noexcept
    : device{ _device },
      descriptor{ std::forward<wgpu::RenderPipelineDescriptor>(_descriptor) },
      scheduler{ _scheduler }
{
}

void RenderPipelineAwaitable::await_suspend(std::coroutine_handle<> handle)
{
    SlotMapHandle slot;
    if (scheduler)
    {
        slot = scheduler->RegisterPending(handle.address());
    }

    auto callback = [handle, &slot, this](wgpu::CreatePipelineAsyncStatus status,
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

        if (scheduler)
        {
            RhiError readyResult = scheduler->MarkReady(slot);
            if (readyResult != RhiError::Success) [[unlikely]]
            {
                result = std::unexpected(readyResult);
            }
        }
        else
        {
            handle.resume();
        }
    };

    device.CreateRenderPipelineAsync(descriptor, wgpu::CallbackMode::AllowSpontaneous, callback);
}

Result<wgpu::RenderPipeline> RenderPipelineAwaitable::await_resume() noexcept
{
    return std::move(result);
}

void ComputePipelineAwaitable::await_suspend(std::coroutine_handle<> handle)
{
    SlotMapHandle slot;
    if (scheduler)
    {
        slot = scheduler->RegisterPending(handle.address());
    }

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

        if (scheduler)
        {
            scheduler->MarkReady(slot);
        }
        else
        {
            handle.resume();
        }
    };
    device.CreateComputePipelineAsync(descriptor, wgpu::CallbackMode::AllowSpontaneous, callback);
}

Result<wgpu::ComputePipeline> ComputePipelineAwaitable::await_resume() noexcept
{
    return std::move(result);
}

} // namespace velox