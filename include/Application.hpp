#pragma once
#ifndef VELOX_APPLICATION_HPP
#define VELOX_APPLICATION_HPP
#include <cstdint>

struct GLFWwindow;

namespace wgpu
{
    class TextureView;
}

namespace velox
{
    class Context;

    enum class InitStatus : uint8_t
    {
        Success = 0,
        Failure = 1
    };

    /**
     * @brief Lifecylcle interface any instance of a Velox application must implement. Takes a context reference in
     * each function to separate those two concerns and make ownership less annoying. Effectively serves as a stub
     * for the key main loop functions we'll need to run a demo/showcase.
    */
    class Application
    {
        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;
    public:
        virtual ~Application();

        // Call after context initializes instance: this will then allow adapter and device
        // init to be passed off to be executed async, as we prefer
        virtual InitStatus OnInstanceInit(Context& context);

        // Called whenever the surface needs to be (re)configured, including
        // once up front with the initial window/canvas size.
        virtual void OnResize(Context& context, uint32_t width, uint32_t height);

        // Called once per frame, before OnRender. dt is in seconds.
        virtual void OnUpdate(Context& context, double dt);

        // Called once per frame. `backbuffer` is the surface's current texture
        // view, already acquired - just record and submit your command buffer.
        virtual void OnRender(Context& context, wgpu::TextureView& backbuffer) = 0;

        // Called once, before the Context is torn down.
        virtual void OnShutdown(Context& context);

    };

    // Anything after we call RunApplication() in main will not be called on web, for now
    void ApplicationMainLoop(Context& context, Application& app);

} // namespace velox

#endif // !VELOX_APPLICATION_HPP
