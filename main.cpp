#include "Context.hpp"
#include <cassert>
#include <string_view>
#include <span>
#include "GLFW/glfw3.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif
#include "HdrTestPatternShader.hpp"

constexpr const char* const shaderSource = R"(
struct VertexOutput
{
    @builtin(position) Position : vec4<f32>,
    @location(0) VertexColor : vec4<f32>
};

@vertex
fn VsMain(@builtin(vertex_index) inVertexIndex : u32) -> VertexOutput
{
    var pos = array<vec2<f32>, 3>
    (
        vec2<f32>(0.0, 0.5),
        vec2<f32>(-0.5, -0.5),
        vec2<f32>(0.5, -0.5)
    );

    var color = array<vec3<f32>, 3>
    (
        vec3<f32>(1.0, 0.0, 0.0),
        vec3<f32>(0.0, 1.0, 0.0),
        vec3<f32>(0.0, 0.0, 1.0)
    );

    var output : VertexOutput;
    output.Position = vec4<f32>(pos[inVertexIndex], 0.0, 1.0);
    output.VertexColor = vec4<f32>(color[inVertexIndex], 1.0);
    return output;
}

const kExposure : f32 = 4.0;
const kTonemap : u32 = 2u; // 0 = raw/hard-clip, 1 = Reinhard, 2 = crude ACES-ish

fn tonemap(c : vec3<f32>, mode : u32) -> vec3<f32>
{
    var result : vec3<f32>;
    if (mode == 0u)
    {
        result = c;
    }
    else if (mode == 1u)
    {
        result = c / (c + vec3<f32>(1.0));
    }
    else if (mode == 2u)
    {
        result = clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), vec3<f32>(0.0), vec3<f32>(1.0));
    }
    return result;
}

@fragment
fn FsMain(in: VertexOutput) -> @location(0) vec4<f32>
{
    var c = in.VertexColor.rgb * kExposure;

    c = tonemap(c, kTonemap);

    return vec4<f32>(c, in.VertexColor.a);
}
)";


struct BufferWriteAwaitable
{
    BufferWriteAwaitable(wgpu::Buffer& buffer, wgpu::Queue& queue, const void* data, size_t size)
        : buffer(buffer), queue(queue), data(data), size(size) {}

    
    bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle)
    {
        // backend keeps a copy of the data until the write is complete, so on return
        // we don't need to keep the data alive
        queue.WriteBuffer(buffer, 0, data, size);
        // commands submitted after this will be executed after the write, e.g commandencoder submissions
        handle.resume();
    }

    void await_resume() const noexcept {}

    wgpu::Buffer& buffer;
    wgpu::Queue& queue;
    const void* data;
    size_t size;
};

// so if queue.WriteBuffer copies data for us, Map() which runs async should not?

enum class MapMode
{  
    Unknown = 0,
    Write,
    Read
};

template<MapMode T>
struct BufferMapAwaitable
{
    wgpu::Buffer& buffer;
    wgpu::Queue& queue;
    // switch mappedData to void* for write, const void* for read
    std::conditional_t<T == MapMode::Write, void*, const void*> mappedData;
    size_t size;
    size_t offset;

    BufferMapAwaitable(wgpu::Buffer& buffer, size_t size, size_t offset)
        : buffer(buffer), queue(queue), size(size), offset(offset) {}

    bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle)
    {
        if constexpr (T == MapMode::Write)
        {
            buffer.MapAsync(wgpu::MapMode::Write, offset, size, wgpu::CallbackMode::AllowSpontaneous,
                [this, handle](wgpu::MapAsyncStatus status, wgpu::StringView message)
                {
                    std::println(stderr, "[velox][BufferMapAwaitable] Buffer mapped");
                    mappedData = buffer.GetMappedRange(offset, size);
                    // we resume the coroutine to return to original caller
                    handle.resume();
                });
        }
        else if constexpr (T == MapMode::Read)
        {
            // need to set mapping mode, based on what mode we actually want
            buffer.MapAsync(wgpu::MapMode::Read, offset, size, wgpu::CallbackMode::AllowSpontaneous,
                [this, handle](wgpu::MapAsyncStatus status, wgpu::StringView message)
                {
                    std::println(stderr, "[velox][BufferMapAwaitable] Buffer mapped");
                    mappedData = buffer.GetConstMappedRange(0, size);
                    // we resume the coroutine to return to original caller
                    handle.resume();
                });
        }
        
    }

    decltype(mappedData) await_resume() const noexcept
    {
        return mappedData;
    }

};

template<MapMode Mode>
struct AsyncMapRequest
{
    
};


// We need to call unmap() after we map a buffer, but the map run synchronously.
// We can use a Session object to manage the lifetime, so that the session maps
// on construction and unmaps on destruction - and we can perform work with the 
// mapped data during the session lifetime
template<MapMode Mode>
struct BufferMapSession
{
private:
    wgpu::Buffer buffer;
    wgpu::Queue& queue;
    size_t size;
    size_t offset;
    using PointerType = std::conditional_t<Mode == MapMode::Read, const void*, void*>;
    PointerType mappedData;
public:

    BufferMapSession(const BufferMapSession&) = delete;
    BufferMapSession& operator=(const BufferMapSession&) = delete;
    ~BufferMapSession() noexcept
    {
        std::println(stderr, "[velox][BufferMapSession] Buffer {} Map Session ended", magic_enum::enum_name(Mode));
        buffer.Unmap();
    }

    PointerType GetMappedData() const noexcept
    {
        return mappedData;
    }

    template<typename T>
    std::span<std::conditional_t<Mode == MapMode::Read, const T, T>> GetDataAs() const noexcept
    {
        // T needs to be standard layout if we want to interpret this as a span of T, otherwise
        // we can't be certain of the layout of T in memory
        static_assert(std::is_standard_layout_v<T>, "T must be standard layout to interpret mapped data as a span of T");
        using MappedType = std::conditional_t<Mode == MapMode::Read, const T, T>;
        return std::span<MappedType>(static_cast<std::add_pointer_t<MappedType>>(mappedData), size / sizeof(T));
    }

    static Task<BufferMapSession<Mode>> Create(wgpu::Buffer& buffer, size_t size, size_t offset = 0)
    {
        const void* mappedData = co_await BufferMapAwaitable<Mode>(buffer, size, offset);
        if (!mappedData)
        {
            std::println(stderr, "[velox][BufferMapSession] Failed to map buffer, returning empty session for continuity.");
            co_return BufferMapSession<Mode>(buffer, size, offset, nullptr);
        }
        co_return BufferMapSession<Mode>(buffer, size, offset, mappedData);
    }

};

struct UniformBuffer
{
    UniformBuffer(wgpu::Device& device, size_t size, std::string_view label = {})
    {
        wgpu::BufferDescriptor bufferDesc = getBufferDescriptor(size, label);
        buffer = device.CreateBuffer(&bufferDesc);
        if (!buffer)
        {
            std::println(stderr, "[velox][UniformBuffer] Failed to create uniform buffer");
            assert(false);
        }
    }

    UniformBuffer(wgpu::Device& device, size_t size, const void* data)
    {
        wgpu::BufferDescriptor bufferDesc = getBufferDescriptor(size, "UniformBuffer");
        bufferDesc.mappedAtCreation = true;
        buffer = device.CreateBuffer(&bufferDesc);
        if (!buffer)
        {
            std::println(stderr, "[velox][UniformBuffer] Failed to create uniform buffer");
            assert(false);
        }
        void* mappedData = buffer.GetMappedRange();
        std::memcpy(mappedData, data, size);
        buffer.Unmap();
    }

    void Update(wgpu::Queue& queue, const void* data, size_t size)
    {
        queue.WriteBuffer(buffer, 0, data, size);
    }

    wgpu::Buffer buffer{};
private:
    wgpu::BufferDescriptor getBufferDescriptor(size_t size, std::string_view label)
    {
        wgpu::BufferDescriptor bufferDesc{};
        bufferDesc.size = size;
        bufferDesc.usage = wgpu::BufferUsage::Uniform |
                           wgpu::BufferUsage::MapRead |
                           wgpu::BufferUsage::MapWrite;

        bufferDesc.mappedAtCreation = false;
        bufferDesc.label = label.data();
        return bufferDesc;
    }
};


void Render(wgpu::Surface& surface, wgpu::Device& device, wgpu::Queue& queue, wgpu::RenderPipeline& pipeline)
{
    wgpu::SurfaceTexture surfaceTexture{};
    surface.GetCurrentTexture(&surfaceTexture);
    wgpu::RenderPassColorAttachment colorAttachment{};
    colorAttachment.view = surfaceTexture.texture.CreateView();
    colorAttachment.loadOp = wgpu::LoadOp::Clear;
    colorAttachment.storeOp = wgpu::StoreOp::Store;
    colorAttachment.clearValue = { 113.0f / 255.0f, 153.0f / 255.0f, 1.0f, 1.0f };

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
    createInfo.InitialWidth = 1280;
    createInfo.InitialHeight = 720;
    createInfo.RequiredFeatures = requestedFeaturesSpan;
    createInfo.FeatureLevel = wgpu::FeatureLevel::Compatibility;
    createInfo.PowerPreference = wgpu::PowerPreference::HighPerformance;
    createInfo.PreferredSurfaceFormat = wgpu::TextureFormat::RGBA16Float;
    createInfo.PreferredColorSpace = wgpu::PredefinedColorSpace::DisplayP3;
    createInfo.PreferredToneMappingMode = wgpu::ToneMappingMode::Extended;
    
    Context context(createInfo);
    // okay, lets try a basic triangle
    using namespace wgpu;

    Task<std::expected<bool, RhiError>> initTask = context.InitWebGPU(createInfo);
    // in async mode, we need to wait for the task to complete before continuing

    ShaderSourceWGSL wgslSource{};
    wgslSource.code = hdrTestPatternShaderSource;
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

    // "playing with buffers" test code
    wgpu::BufferDescriptor bufferDesc{};
    bufferDesc.label = "GpuSide TestBuffer";
    bufferDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc;
    bufferDesc.size = 512; // demo says 16 bytes but i think bumping it higher will actually copy
    bufferDesc.mappedAtCreation = false;
    wgpu::Buffer testBuffer = context.GetDevice().CreateBuffer(&bufferDesc);
    if (!testBuffer)
    {
        std::println(stderr, "[velox][main] Failed to create test buffer");
        return -1;
    }

    // just change label to make a second buffer
    bufferDesc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    bufferDesc.label = "GpuSide TestBuffer2";
    wgpu::Buffer testBuffer2 = context.GetDevice().CreateBuffer(&bufferDesc);
    if (!testBuffer2)
    {
        std::println(stderr, "[velox][main] Failed to create test buffer 2");
        return -1;
    }

    // destroy() frees the backing memory, removal of the handle is only done on
    // loss of all refs I think? wgpu-cpp adds some confusion for me here, too used to vulkan C style handles
    // testBuffer2.Destroy();

    std::vector<uint32_t> testData(bufferDesc.size / sizeof(uint32_t));
    for (uint32_t i = 0; i < testData.size(); ++i)
    {
        testData[i] = i;
    }

    auto queue = context.GetQueue();
    queue.WriteBuffer(testBuffer, 0, testData.data(), testData.size() * sizeof(uint32_t));
    CommandEncoder encoder = context.GetDevice().CreateCommandEncoder();
    encoder.CopyBufferToBuffer(testBuffer, 0, testBuffer2, 0, testData.size() * sizeof(uint32_t));
    wgpu::CommandBuffer commandBuffer = encoder.Finish();
    queue.Submit(1, &commandBuffer);

    {
        BufferReadSession readSession(testBuffer2, queue, testData.size() * sizeof(uint32_t));
        const void* mappedData = readSession.MappedData;
        std::vector<uint32_t> readBackData(testData.size());
        std::memcpy(readBackData.data(), mappedData, testData.size() * sizeof(uint32_t));
        // now compare
        if (readBackData != testData)
        {
            std::println(stderr, "[velox][main] Read back data does not match written data");
            return -1;
        }
    }

    Task<const void*> mapTask = BufferMapForReadTask(testBuffer2, queue, testData.size() * sizeof(uint32_t));
    mapTask.Wait();
    const void* mappedData = mapTask.GetResult();

    // copy mapped data back to a vector to verify it matches what we wrote
    std::vector<uint32_t> readBackData(testData.size());
    std::memcpy(readBackData.data(), mappedData, testData.size() * sizeof(uint32_t));
    // now compare
    if (readBackData != testData)
    {
        std::println(stderr, "[velox][main] Read back data does not match written data");
        return -1;
    }
    // unmap the buffer (which can happen not-async?)
    testBuffer2.Unmap();

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

