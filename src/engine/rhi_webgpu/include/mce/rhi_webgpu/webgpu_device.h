#pragma once
// mce_rhi_webgpu: the WebGPU RenderDevice for the browser (ADR-0039), over Dawn's emdawnwebgpu
// bindings to the browser's navigator.gpu.
//
// The device draws into the window's canvas (SDL's Emscripten canvas, "#canvas"); the window must
// outlive the device. All device calls happen on the main thread. Creation waits for the browser's
// adapter and device, which needs the ASYNCIFY build the web presets use (cmake/MceWeb.cmake).

#include "mce/platform/window.h"
#include "mce/rhi/render_device.h"

namespace mce::rhi::webgpu {

Result<mem::UniquePtr<RenderDevice>> create_device(platform::Window& window, const DeviceDesc& desc = {});

}  // namespace mce::rhi::webgpu
