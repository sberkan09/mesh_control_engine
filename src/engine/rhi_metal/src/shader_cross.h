#pragma once
// GLSL 450 core -> SPIR-V (glslang, OpenGL client semantics) -> MSL (SPIRV-Cross), ADR-0027.
//
// The engine's shaders are written against the GL backend's conventions (rhi/types.h): separable
// stages, std140 uniform blocks at the fixed banks, combined samplers at 6+, SSBOs, gl_BaseInstanceARB.
// translate_glsl() maps those onto fixed Metal argument indices so the device can bind by bank number:
//
//   uniform block `binding = b`        -> [[buffer(b)]]                 (b < kStorageBufferBase)
//   storage buffer `binding = b`       -> [[buffer(kStorageBufferBase + b)]]
//   sampler `binding = s`              -> [[texture(s)]], [[sampler(s)]]
//   vertex attributes (location = n)   -> [[attribute(n)]] fed from [[buffer(kVertexBufferIndex)]]
//
// Clip space is fixed up to Metal's z in [0, 1]. Vertex stages carry a function constant
// (kFlipYConstant) that negates gl_Position.y: the device sets it when rendering into a texture, so
// texture memory row 0 holds NDC y = -1 exactly as GL lays it out (rhi-renderer.md: row 0 is v = 0).
//
// Pure C++ and API-free: the unit tests run it without a GPU.

#include "mce/fixed_string.h"
#include "mce/result.h"

namespace mce::rhi::metal {

enum class ShaderStage : u8 { Vertex, Fragment };

inline constexpr u32 kStorageBufferBase = 8;
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
        o.text_ = nullptr;
        o.size_ = 0;
    }
    MslSource& operator=(MslSource&& o) noexcept;
    ~MslSource();

    const char* c_str() const { return text_ ? text_ : ""; }
    usize size() const { return size_; }
    StringView view() const { return {c_str(), size_}; }

    static MslSource copy_of(StringView s);

private:
    char* text_ = nullptr;
    usize size_ = 0;
};

// Translates one stage. On failure the compiler log lands in `log` and the Error carries
// Errc::ShaderCompile with the shader's debug name as its path.
Result<MslSource> translate_glsl(ShaderStage stage, StringView glsl, const char* debug_name, FixedString<1024>& log);

}  // namespace mce::rhi::metal
