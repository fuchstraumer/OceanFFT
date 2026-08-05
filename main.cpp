#include "Context.hpp"
#include <cassert>
#include <string_view>
#include <span>
#include "GLFW/glfw3.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

constexpr const char* const shaderSource = R"(
@vertex
fn VsMain(@builtin(vertex_index) inVertexIndex : u32) -> @builtin(position) vec4<f32>
{
    var pos = array<vec2<f32>, 3>
    (
        vec2<f32>(0.0, 0.5),
        vec2<f32>(-0.5, -0.5),
        vec2<f32>(0.5, -0.5)
    );
    return vec4<f32>(pos[inVertexIndex], 0.0, 1.0);
}

@fragment
fn FsMain() -> @location(0) vec4<f32>
{
    return vec4<f32>(0.0, 0.4, 0.0, 1.0);
}
)";

void Render(wgpu::Surface& surface, wgpu::Device& device, wgpu::Queue& queue, wgpu::RenderPipeline& pipeline)
{
    wgpu::SurfaceTexture surfaceTexture{};
    surface.GetCurrentTexture(&surfaceTexture);
    wgpu::RenderPassColorAttachment colorAttachment{};
    colorAttachment.view = surfaceTexture.texture.CreateView();
    colorAttachment.loadOp = wgpu::LoadOp::Clear;
    colorAttachment.storeOp = wgpu::StoreOp::Store;

    wgpu::RenderPassDescriptor renderPassDesc{};
    renderPassDesc.colorAttachmentCount = 1;
    renderPassDesc.colorAttachments = &colorAttachment;

    wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    wgpu::RenderPassEncoder renderPass = encoder.BeginRenderPass(&renderPassDesc);
    renderPass.SetPipeline(pipeline);
    renderPass.Draw(3);
    renderPass.End();

    wgpu::CommandBuffer commandBuffer = encoder.Finish();
    queue.Submit(1, &commandBuffer);
}

struct MainLoopState
{
    velox::Context* context;
    wgpu::RenderPipeline pipeline;
};

#ifdef __EMSCRIPTEN__
void EmMainLoopArg(void* arg)
{
    using namespace velox;
    MainLoopState* mainLoopState = static_cast<MainLoopState*>(arg);
    velox::Context* context = mainLoopState->context;
    Render(context->GetSurface(), context->GetDevice(), context->GetQueue(), mainLoopState->pipeline);
    context->Present();
    context->GetInstance().ProcessEvents();
}
#endif

int main()
{
    using namespace velox;
    ContextCreateInfo createInfo{};
    createInfo.ApplicationName = "Velox Test App";
    wgpu::FeatureName requestedFeatures[] = 
    {
        wgpu::FeatureName::ShaderF16,
        wgpu::FeatureName::Subgroups
    };
    std::span<wgpu::FeatureName> requestedFeaturesSpan(requestedFeatures);
    createInfo.RequiredFeatures = requestedFeaturesSpan;
    createInfo.FeatureLevel = wgpu::FeatureLevel::Compatibility;
    createInfo.PowerPreference = wgpu::PowerPreference::HighPerformance;
    createInfo.PreferredSurfaceFormat = wgpu::TextureFormat::RGB10A2Unorm;
    
    Context context(createInfo);
    // okay, lets try a basic triangle
    using namespace wgpu;

    ShaderSourceWGSL wgslSource{};
    wgslSource.code = shaderSource;
    ShaderModuleDescriptor shaderDesc{};
    shaderDesc.nextInChain = &wgslSource;
    ShaderModule shaderModule = context.GetDevice().CreateShaderModule(&shaderDesc);
    if (!shaderModule)
    {
        std::println(stderr, "[velox][main] Failed to create shader module");
        return -1;
    }
    
    ColorTargetState colorTarget{};
    colorTarget.format = context.GetSurfaceFormat();

    VertexState vertexState{};
    vertexState.module = shaderModule;
    vertexState.bufferCount = 0;
    vertexState.buffers = nullptr;
    vertexState.entryPoint = "VsMain";

    PrimitiveState primitiveState{};
    primitiveState.topology = PrimitiveTopology::TriangleList;
    primitiveState.cullMode = CullMode::None;
    primitiveState.frontFace = FrontFace::CCW;

    FragmentState fragmentState{};
    fragmentState.module = shaderModule;
    fragmentState.targetCount = 1;
    fragmentState.targets = &colorTarget;
    fragmentState.entryPoint = "FsMain";

    RenderPipelineDescriptor pipelineDesc{};
    pipelineDesc.label = "TestTrianglePipeline";
    pipelineDesc.nextInChain = nullptr;
    pipelineDesc.vertex = vertexState;
    pipelineDesc.fragment = &fragmentState;
    pipelineDesc.primitive = primitiveState;

    RenderPipeline pipeline = context.GetDevice().CreateRenderPipeline(&pipelineDesc);
    if (!pipeline)
    {
        std::println(stderr, "[velox][main] Failed to create render pipeline");
        return -1;
    }

#ifndef __EMSCRIPTEN__
    while (!glfwWindowShouldClose(context.GetNativeWindow()))
    {
        glfwPollEvents();
        Render(context.GetSurface(), context.GetDevice(), context.GetQueue(), pipeline);
        context.Present();
        context.GetInstance().ProcessEvents();
    }
#else
    MainLoopState mainLoopState{ &context, pipeline };
    emscripten_set_main_loop_arg(EmMainLoopArg, &mainLoopState, 0, true);
#endif
    
    return 0;
}

