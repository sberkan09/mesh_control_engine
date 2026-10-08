#pragma once
// rhi::create_default_device — the platform's native RenderDevice (docs/architecture/macos-metal.md §2).
//
// Hosts (samples, player, editor, GPU tests) call this instead of naming a backend: OpenGL 4.5 core
// (mce_rhi_gl) on Windows and Linux, Metal (mce_rhi_metal) on macOS. Same contract as the backends'
// own create_device: the window must be created with WindowDesc::opengl (a GPU-capable surface) and
// outlive the device, and every device call happens on the creating thread.

#include "mce/platform/window.h"
#include "mce/rhi/render_device.h"

namespace mce::rhi {

Result<mem::UniquePtr<RenderDevice>> create_default_device(platform::Window& window, const DeviceDesc& desc = {});

}  // namespace mce::rhi
