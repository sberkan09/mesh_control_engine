#pragma once
// GLSL 450 core (GL conventions) -> SPIR-V -> WGSL for WebGPU (ADR-0039 §3-§4).
//
// The engine's shaders are written for the GL backend (rhi/types.h): std140 uniform blocks at the
// fixed banks 0..3, combined samplers at 0..15, std430 storage blocks at 0..15, gl_VertexID and
// gl_InstanceID with GL meaning, clip z in [-w, w]. compile_glsl() keeps that source unchanged:
//
//   - gl_VertexID -> gl_VertexIndex (both include the base vertex); gl_InstanceID ->
//     gl_InstanceIndex, which WebGPU counts from the draw's first instance (WGSL has no base
//     instance builtin); every engine draw that reads gl_InstanceID uses base instance 0;
//   - vertex stages get an epilogue that maps clip z to [0, w] and negates gl_Position.y, so a
//     render target's memory holds GL's layout (row 0 = NDC y = -1) under WebGPU's y-down framebuffer;
//   - glslang compiles with Vulkan rules, then the DescriptorSet/Binding decorations are rewritten
//     into one fixed layout, so a GL binding number is also the WebGPU binding number:
//
//       @group(0) uniform bank b                      @binding(b)       b in 0..3
//       @group(1) sampled texture of GL sampler s     @binding(s)       s in 0..15
//                 its sampler                         @binding(16 + s)  (Tint splits the combined sampler)
//       @group(2) storage buffer of GL binding n      @binding(n)       n in 0..15
//       @group(3) storage image of GL image unit u    @binding(u)       compute only
//
//   - Tint reads the SPIR-V and writes WGSL. The entry point is always `main`.
//
// StageResources lists what the stage declares, so the device builds its bind group layouts from
// it. Pure C++ and API-free: the unit tests run it without a GPU.

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/result.h"

namespace mce::rhi::webgpu {

enum class ShaderStage : u8 { Vertex, Fragment, Compute };

// What a sampled binding expects, so the layout gets the right view dimension and sample type and an
// unbound slot gets a dummy of the right kind.
enum class SamplerKind : u8 { Tex2D, Tex2DArray, Shadow2D, Shadow2DArray };

inline constexpr u32 kMaxSamplers = 16;        // GL sampler bindings 0..15
inline constexpr u32 kMaxStorageBindings = 16; // GL storage bindings 0..15 (rhi::bank)
inline constexpr u32 kMaxImageUnits = 8;       // GL image units (bind_storage_image)
inline constexpr u32 kMaxUniformBanks = 4;
inline constexpr u32 kSamplerBindingBase = 16; // @group(1): sampler of texture s at 16 + s

inline constexpr u32 kGroupUniforms = 0;
inline constexpr u32 kGroupTextures = 1;
inline constexpr u32 kGroupStorage = 2;
inline constexpr u32 kGroupImages = 3;

struct StageResources {
    u32 sampler_mask = 0;                     // bit s: GL sampler binding s is declared
    SamplerKind sampler_kind[kMaxSamplers] = {};
    u32 storage_mask = 0;                     // bit n: storage binding n is declared
    u32 storage_rw_mask = 0;                  // ... and written (compute only; graphics stages are read-only)
    u32 uniform_mask = 0;                     // bit b: uniform bank b is declared
    u32 image_mask = 0;                       // bit u: storage image unit u is declared (compute)
    u32 image_read_only_mask = 0;             // ... as `readonly`
    u32 image_write_only_mask = 0;            // ... as `writeonly`
    u32 local_size[3] = {1, 1, 1};
};

// Translates one stage into NUL-terminated WGSL in `wgsl`. On failure the compiler log lands in
// `log` and the Error carries Errc::ShaderCompile with the shader's debug name as its path.
ResultV compile_glsl(ShaderStage stage, StringView glsl, const char* debug_name, Array<char>& wgsl,
                     StageResources& resources, FixedString<1024>& log);

}  // namespace mce::rhi::webgpu
