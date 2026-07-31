#pragma once
#ifndef COMMON_WEBGPU_HPP
#define COMMON_WEBGPU_HPP

#include <expected>
// backend for us is always dawn
#include <webgpu/webgpu_cpp.h>

// Assert-style wrappers for various WGPU functions using std::expected
// https://github.com/dj2/Dusk/blob/main/src/common/wgpu.h <- main source

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

#endif //!COMMON_WEBGPU_HPP