#include "mce/rhi/default_device.h"

#if defined(MCE_RHI_SDLGPU_ONLY)
// Android/iOS (ADR-0038): mce_rhi_sdlgpu is the only backend.
#elif defined(MCE_RHI_WEBGPU_ONLY)
#include "mce/rhi_webgpu/webgpu_device.h"  // the browser (ADR-0039): mce_rhi_webgpu is the only backend
#elif defined(__APPLE__)
#include "mce/rhi_metal/metal_device.h"
#else
#include "mce/rhi_gl/gl_device.h"
#endif
#if defined(MCE_HAS_RHI_SDLGPU)
#include "mce/rhi_sdlgpu/sdlgpu_device.h"
#endif

namespace mce::rhi {

Result<mem::UniquePtr<RenderDevice>> create_default_device(platform::Window& window, const DeviceDesc& desc) {
    if (platform::gpu_api() == platform::GpuApi::SdlGpu) {
#if defined(MCE_HAS_RHI_SDLGPU)
        return sdlgpu::create_device(window, desc);
#else
        return Error(Errc::Unsupported, "rhi.default", "MCE_RHI=sdlgpu, but this build has no mce_rhi_sdlgpu");
#endif
    }
#if defined(MCE_RHI_SDLGPU_ONLY)
    return sdlgpu::create_device(window, desc);
#elif defined(MCE_RHI_WEBGPU_ONLY)
    return webgpu::create_device(window, desc);
#elif defined(__APPLE__)
    return metal::create_device(window, desc);
#else
    return gl::create_device(window, desc);
#endif
}

}  // namespace mce::rhi
