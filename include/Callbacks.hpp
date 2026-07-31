#pragma once
#ifndef WEBGPU_CALLBACKS_HPP
#define WEBGPU_CALLBACKS_HPP
#include "WebGpu.hpp"

// Webgpu uses callbacks for most of it's "async" operations, so the core callbacks we need are all defined
// here in this file.

/**
 * Callback used when creating/requesting a GPU adapter.
 */
void AdapterRequestCallback(wgpu::RequestAdapterStatus status,
                            wgpu::Adapter adapter,
                            wgpu::StringView message,
                            wgpu::Adapter* data);

#endif //!WEBGPU_CALLBACKS_HPP