#include "Future.hpp"

namespace velox
{

RenderPipelineFuture RequestRenderPipeline(Context* _ctxt,
                                                                    wgpu::Device _device,
                                                                    wgpu::RenderPipelineDescriptor _descr)
{
    auto coroutine = [](Context* ctxt, wgpu::Device _device, wgpu::RenderPipelineDescriptor _descr)
    {
        
    }
}
}