#pragma once
// mce_rhi_sdlgpu: the SDL3 GPU RenderDevice (ADR-0038) — Vulkan on Linux/Android, Metal on iOS.
//
// The device claims Window::native_handle() for SDL GPU. The window must have been created with
// WindowDesc::opengl while platform::gpu_api() is GpuApi::SdlGpu (a Vulkan- or Metal-capable
// surface) and must outlive the device. All device calls happen on the thread that created it.

#include "mce/platform/window.h"
#include "mce/rhi/render_device.h"

namespace mce::rhi::sdlgpu {

Result<mem::UniquePtr<RenderDevice>> create_device(platform::Window& window, const DeviceDesc& desc = {});

}  // namespace mce::rhi::sdlgpu
