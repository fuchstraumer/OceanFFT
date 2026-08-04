#pragma once
#ifndef VELOX_ERRORS_HPP
#define VELOX_ERRORS_HPP
#include <string>
#include <expected>
#include <print>
#include <format>
#include <type_traits>
// backend for us is always dawn
#include <webgpu/webgpu_cpp.h>
#include <iostream>

// Assert-style wrappers for various WGPU functions using std::expected. A lot of this comes from the following repo,
// but also this is probably just going to  be a common pattern across most WebGPU apps I think.... 
// (im mostly happy to see someone else using concepts and constraints in the wild!)
// https://github.com/dj2/Dusk/blob/main/src/common/wgpu.h
// https://github.com/dj2/Dusk/blob/main/src/common/expected.h

namespace velox
{

    enum class RhiError : uint32_t
    {
        // WebGPU errors
        AdapterRequestFailed = 1,
        DeviceRequestFailed = 2,
        SurfaceCreationFailed = 3,
        SurfaceConfigurationFailed = 4,
        SurfaceAcquireFailed = 5,
        SurfacePresentFailed = 6,
        // Dawn-specific errors
        // GLFW errors
        GLFWInitFailed = 300,
        GLFWWindowCreationFailed = 301,
        // Imgui errors
        ImguiContextInitFailed = 400,
    };

    #define WGPU_TRY(expr) \
        do \
        { \
            wgpu::Status result = (expr); \
            if (result != wgpu::Status::Success) \
            { \
                const std::string msg = std::format("WGPU_TRY failed: {} with status {} \n", #expr, static_cast<int>(result)); \
                return std::unexpected(msg); \
            } \
        } while (false); \

    template<typename T, typename E> requires(!std::is_void_v<T>)
    T ValidOrExit(std::expected<T, E> result)
    {
        if (!result)
        {
            std::string errorMessage = "Error: " + std::to_string(static_cast<int>(result.error()));
            std::cerr << errorMessage << std::endl;
            std::exit(1); // should shore this up with better exit codes and callbacks eventually
        }
        return result.value();
    }

    template<typename T, typename E> requires(std::is_void_v<T>)
    void ValidOrExit(std::expected<T, E> result)
    {
        if (!result)
        {
            std::println(stderr, "Error: {}", result.error());
            std::exit(1);
        }
    }
}

#endif //!VELOX_ERRORS_HPP