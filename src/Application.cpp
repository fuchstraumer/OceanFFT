#include "Application.hpp"
#include "Context.hpp"
#include <chrono>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

namespace
{

}

namespace velox
{

    InitStatus Application::OnInstanceInit(Context& context)
    {
        return InitStatus::Success;
    }

    void Application::OnResize(Context& context, uint32_t width, uint32_t height)
    {
        // default implementation does nothing
    }

    void Application::OnUpdate(Context& context, double dt)
    {
        // default implementation does nothing
    }

    void Application::OnShutdown(Context& context)
    {
        // default implementation does nothing
    }

#ifdef __EMSCRIPTEN__

    void ApplicationMainLoop(Context& context, Application& app)
    {

    }

#else // ndef __EMSCRIPTEN__

    void ApplicationMainLoop(Context& context, Application& app, GLFWwindow* window)
    {

    }

#endif // __EMSCRIPTEN__

} // namespace velox