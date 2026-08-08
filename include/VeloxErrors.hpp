#pragma once
#ifndef VELOX_ERRORS_HPP
#define VELOX_ERRORS_HPP
#include <expected>
#include <type_traits>
#ifndef NDEBUG
#include <print>
#include <magic_enum/magic_enum.hpp>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// Assert-style wrappers for various WGPU functions using std::expected. A lot of this comes from the
// following repo, but also this is probably just going to  be a common pattern across most WebGPU apps I
// think.... (im mostly happy to see someone else using concepts and constraints in the wild!)
// https://github.com/dj2/Dusk/blob/main/src/common/wgpu.h
// https://github.com/dj2/Dusk/blob/main/src/common/expected.h

namespace velox
{

enum class RhiError : uint64_t
{
    // WebGPU errors
    Success = 0,
    InstanceRequestFailed = 10,
    AdapterRequestFailed = 11,
    DeviceRequestFailed = 12,
    SurfaceCreationFailed = 13,
    SurfaceConfigurationFailed = 14,
    SurfaceAcquireFailed = 15,
    SurfacePresentFailed = 16,
    BufferMapFailed = 17,
    PipelineCreationFailed = 18,
    // Start async function specific error codes (as needed)
    AsyncCallbackCanceled = 50,
    MapAsyncAborted = 51, // what exactly does webgpu mean by this?
    AsyncCreatePipelineValidationFailed = 52,
    AsyncCreatePipelineInternalError = 53,

    MapAsyncFailed = 7,
    // Dawn-specific errors
    // GLFW errors
    GLFWInitFailed = 300,
    GLFWWindowCreationFailed = 301,
    // Imgui errors
    ImguiContextInitFailed = 400,
};

template<typename T, typename E> requires(!std::is_void_v<T>)
T ValidOrExit(std::expected<T, E> result)
{
    if (!result)
    {
#ifndef NDEBUG
        std::println(stderr, "ValidOrExit failure with error {}", magic_enum::enum_name(result.error()));
#endif
#ifdef __EMSCRIPTEN__
        emscripten_force_exit(1);
#else
        std::exit(1);
#endif
    }
    return result.value();
}

template<typename T, typename E> requires(std::is_void_v<T>)
void ValidOrExit(std::expected<T, E> result)
{
    if (!result)
    {
#ifndef NDEBUG
        std::println(stderr, "ValidOrExit failure: {}", magic_enum::enum_name(result.error()));
#endif
#ifdef __EMSCRIPTEN__
        emscripten_force_exit(1);
#else
        std::exit(1);
#endif
    }
}

} // namespace velox

#endif //! VELOX_ERRORS_HPP
