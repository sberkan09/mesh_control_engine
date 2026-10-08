#include "mce/rhi/default_device.h"

#if defined(__APPLE__)
#include "mce/rhi_metal/metal_device.h"
#else
#include "mce/rhi_gl/gl_device.h"
#endif

namespace mce::rhi {

Result<mem::UniquePtr<RenderDevice>> create_default_device(platform::Window& window, const DeviceDesc& desc) {
#if defined(__APPLE__)
    return metal::create_device(window, desc);
#else
    return gl::create_device(window, desc);
#endif
}

}  // namespace mce::rhi
