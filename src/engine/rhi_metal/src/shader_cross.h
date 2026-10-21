#pragma once
// GLSL 450 core -> SPIR-V (glslang, OpenGL client semantics) -> MSL (SPIRV-Cross), ADR-0028.
//
// The engine's shaders are written against the GL backend's conventions (rhi/types.h): separable
// stages, std140 uniform blocks at the fixed banks, combined samplers at 6+, SSBOs, gl_BaseInstanceARB.
// translate_glsl() maps those onto fixed Metal argument indices so the device can bind by bank number:
//
//   uniform block `binding = b`        -> [[buffer(b)]]                 (b < kStorageBufferBase)
//   storage buffer `binding = b`       -> [[buffer(kStorageBufferBase + b)]]
//   sampler `binding = s`              -> [[texture(s)]], [[sampler(s)]]
//   storage image `binding = u`        -> [[texture(kStorageImageBase + u)]]  (compute, image unit u)
//   vertex attributes (location = n)   -> [[attribute(n)]] fed from [[buffer(kVertexBufferIndex)]]
//
// Compute stages (M-VISUAL 1) keep the same map; Metal takes the work-group size at dispatch time, so
// the translation reports the shader's `local_size` alongside the MSL (MslSource::local_size).
//
// Clip space is fixed up to Metal's z in [0, 1]. Vertex stages carry a function constant
// (kFlipYConstant) that negates gl_Position.y: the device sets it when rendering into a texture, so
// texture memory row 0 holds NDC y = -1 exactly as GL lays it out (rhi-renderer.md: row 0 is v = 0).
//
// Pure C++ and API-free: the unit tests run it without a GPU.

#include "mce/fixed_string.h"
#include "mce/result.h"

namespace mce::rhi::metal {

enum class ShaderStage : u8 { Vertex, Fragment, Compute };

inline constexpr u32 kStorageBufferBase = 8;
inline constexpr u32 kStorageImageBase = 16;  // image unit u -> [[texture(16 + u)]], after the 16 sampler slots
inline constexpr u32 kVertexBufferIndex = 30;
inline constexpr u32 kFlipYConstant = 0;  // [[function_constant(0)]] bool, vertex stages only
inline constexpr const char* kEntryPoint = "main0";

// Owns the generated MSL text (heap). Movable, not copyable.
class MslSource {
public:
    MslSource() = default;
    MslSource(const MslSource&) = delete;
    MslSource& operator=(const MslSource&) = delete;
    MslSource(MslSource&& o) noexcept : text_(o.text_), size_(o.size_) {
        for (u32 i = 0; i < 3; ++i) local_size[i] = o.local_size[i];
        o.text_ = nullptr;
        o.size_ = 0;
    }
    MslSource& operator=(MslSource&& o) noexcept;
    ~MslSource();

    const char* c_str() const { return text_ ? text_ : ""; }
    usize size() const { return size_; }
    StringView view() const { return {c_str(), size_}; }

    static MslSource copy_of(StringView s);

    u32 local_size[3] = {1, 1, 1};  // compute stages: the shader's work-group size

private:
    char* text_ = nullptr;
    usize size_ = 0;
};

// Translates one stage. On failure the compiler log lands in `log` and the Error carries
// Errc::ShaderCompile with the shader's debug name as its path.
Result<MslSource> translate_glsl(ShaderStage stage, StringView glsl, const char* debug_name, FixedString<1024>& log);

}  // namespace mce::rhi::metal
