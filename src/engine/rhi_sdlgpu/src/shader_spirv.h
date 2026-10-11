#pragma once
// GLSL 450 core (GL conventions) -> SPIR-V for SDL GPU's Vulkan driver (ADR-0038 §3-§4).
//
// The engine's shaders are written for the GL backend (rhi/types.h): std140 uniform blocks at the
// fixed banks 0..3, combined samplers at 6..15, std430 storage blocks at 0..7, gl_VertexID and
// gl_InstanceID with GL meaning, clip z in [-w, w]. compile_glsl() keeps that source unchanged:
//
//   - gl_VertexID -> gl_VertexIndex (both include the base vertex); gl_InstanceID ->
//     (gl_InstanceIndex - gl_BaseInstanceARB), because Vulkan's index includes the base instance
//     and GL's does not;
//   - vertex stages get an epilogue that maps clip z to [0, w] and negates gl_Position.y, so a
//     render target's memory holds GL's layout (row 0 = NDC y = -1) under SDL's y-up viewport;
//   - after glslang, the DescriptorSet/Binding decorations are rewritten into SDL's fixed layout:
//
//       vertex   set 0: samplers (compacted, by GL binding), then storage buffers; set 1: uniform bank b
//       fragment set 2: samplers, then storage buffers;                            set 3: uniform bank b
//
// The returned StageResources maps each SDL index back to its GL binding, so the device binds by
// bank number. Pure C++ and API-free: the unit tests run it without a GPU.

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/result.h"

namespace mce::rhi::sdlgpu {

enum class ShaderStage : u8 { Vertex, Fragment };

// What a sampler binding expects, so an unbound slot can get a dummy of the right kind.
enum class SamplerKind : u8 { Tex2D, Tex2DArray, Shadow2D, Shadow2DArray };

inline constexpr u32 kMaxStageSamplers = 16;
inline constexpr u32 kMaxStageStorage = 8;
inline constexpr u32 kMaxUniformBanks = 4;

struct StageResources {
    u8 sampler_slot[kMaxStageSamplers] = {};  // SDL sampler index i <- GL sampler binding
    SamplerKind sampler_kind[kMaxStageSamplers] = {};
    u8 sampler_count = 0;
    u8 storage_binding[kMaxStageStorage] = {};  // SDL storage index i <- GL storage binding
    u8 storage_count = 0;
    u8 uniform_count = 0;  // highest uniform bank used + 1
};

// Translates one stage. On failure the compiler log lands in `log` and the Error carries
// Errc::ShaderCompile with the shader's debug name as its path.
ResultV compile_glsl(ShaderStage stage, StringView glsl, const char* debug_name, Array<u32>& spirv,
                     StageResources& resources, FixedString<1024>& log);

}  // namespace mce::rhi::sdlgpu
