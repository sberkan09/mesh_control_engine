#pragma once
// mce_rhi_gl: creates the OpenGL 4.5 core RenderDevice for a platform window.
//
// The device creates (and owns) the GL context from Window::native_handle() — the sanctioned
// SDL leak (ARCHITECTURE §1.2). The window must have been created with WindowDesc::opengl and
// must outlive the device. All device calls happen on the thread that created it.

#include "mce/platform/window.h"
#include "mce/rhi/render_device.h"

namespace mce::rhi::gl {

Result<mem::UniquePtr<RenderDevice>> create_device(platform::Window& window, const DeviceDesc& desc = {});

}  // namespace mce::rhi::gl
