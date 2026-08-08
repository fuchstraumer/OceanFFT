#include "AsyncTasks.hpp"

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

    Result<wgpu::Adapter> AdapterAwaitable::await_resume()
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


}