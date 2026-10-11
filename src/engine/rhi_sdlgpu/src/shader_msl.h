#pragma once
// SPIR-V (from compile_glsl, SDL's descriptor layout) -> MSL for SDL GPU's Metal driver: iOS (ADR-0038,
// M-MOBILE step 7) and macOS when MCE_RHI=sdlgpu.
//
// The SPIR-V already carries GL's conventions fixed up for SDL GPU (clip z, y flip, gl_VertexID and
// gl_InstanceID meaning, shader_spirv.h), and SDL GPU keeps those conventions identical across its
// drivers, so this is a plain SPIRV-Cross pass that pins every resource to the Metal index SDL binds:
//
//   sampler i (set 0/2, binding i)                -> [[texture(i)]], [[sampler(i)]]
//   uniform bank b (set 1/3, binding b)           -> [[buffer(b)]]
//   storage buffer j (set 0/2, binding n + j)     -> [[buffer(uniform_count + j)]]
//   vertex attributes                             -> [[attribute(location)]] (SDL's vertex descriptor)
//
// The entry point is kMslEntryPoint. Pure C++ and API-free: the unit tests run it without a GPU.

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/result.h"
#include "shader_spirv.h"

namespace mce::rhi::sdlgpu {

inline constexpr const char* kMslEntryPoint = "main0";

enum class MslPlatform : u8 { iOS, macOS };

// MSL source text for one stage; `msl` is cleared first and NUL-terminated on success.
ResultV spirv_to_msl(ShaderStage stage, const Array<u32>& spirv, const StageResources& resources, MslPlatform platform,
                     const char* debug_name, Array<char>& msl, FixedString<1024>& log);

}  // namespace mce::rhi::sdlgpu
