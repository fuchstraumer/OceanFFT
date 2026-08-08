#pragma once
#ifndef VELOX_ASYNC_FUTURE_HPP
#define VELOX_ASYNC_FUTURE_HPP
#include "AsyncTasks.hpp"

namespace velox
{

struct Scheduler;

/* Maybe need a beter name, but this is the type returned to callers of our async functions. */
template<typename T>
class Future
{
    Scheduler* scheduler;
    SlotMapHandle handle;

public:
    std::optional<Result<T>> TryGet();
};

using RenderPipelineFuture = Future<Result<wgpu::RenderPipeline>>;
using ComputePipelineFuture = Future<Result<wgpu::ComputePipeline>>;
using MapReadFuture = Future<Result<const void*>>;
using MapWriteFuture = Future<Result<void*>>;

RenderPipelineFuture RequestRenderPipeline(Scheduler* _ctxt,
                                           wgpu::Device _device,
                                           wgpu::RenderPipelineDescriptor _descr);

ComputePipelineFuture RequestComputePipeline(Scheduler* _ctxt,
                                             wgpu::Device _device,
                                             wgpu::ComputePipelineDescriptor _descr);

MapReadFuture RequestMapBufferReadAsync(Scheduler* _ctxt,
                                        wgpu::Device _device,
                                        wgpu::Buffer _buffer,
                                        size_t size,
                                        size_t offset = 0u);

MapWriteFuture RequestMapBufferWritesync(Scheduler* _ctxt,
                                         wgpu::Device _device,
                                         wgpu::Buffer _buffer,
                                         size_t size,
                                         size_t offset = 0u);

}

#endif // !VELOX_ASYNC_FUTURE_HPP
