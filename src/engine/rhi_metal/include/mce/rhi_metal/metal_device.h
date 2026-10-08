#pragma once
// mce_rhi_metal: creates the Metal RenderDevice for a platform window (macOS, ADR-0028).
//
// The device attaches a CAMetalLayer to Window::native_handle() through SDL's Metal view (the
// sanctioned SDL leak, ARCHITECTURE §1.2). The window must have been created Metal-capable
// (WindowDesc::opengl on macOS selects SDL_WINDOW_METAL) and must outlive the device. All device
// calls happen on the thread that created it.
//
// Shaders stay GLSL 450 (rhi::ShaderSource): pipelines translate them to MSL at creation.

#include "mce/platform/window.h"
#include "mce/rhi/render_device.h"

namespace mce::rhi::metal {

Result<mem::UniquePtr<RenderDevice>> create_device(platform::Window& window, const DeviceDesc& desc = {});

}  // namespace mce::rhi::metal
