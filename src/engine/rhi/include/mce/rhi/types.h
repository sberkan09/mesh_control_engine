#pragma once
// mce_rhi handles, enums and descriptors (rhi-renderer.md §2.1, §2.3).
//
// Handles are core generational handles (never raw pointers across frames). The null handle
// (generation 0) has one special meaning: as a pass attachment it names the window backbuffer.

#include <optional>

#include "mce/memory/handle.h"
#include "mce/types.h"

namespace mce::rhi {

struct BufferTag;
struct TextureTag;
struct SamplerTag;
struct PipelineTag;

using BufferHandle = Handle<BufferTag>;
using TextureHandle = Handle<TextureTag>;
using SamplerHandle = Handle<SamplerTag>;
using PipelineHandle = Handle<PipelineTag>;

struct Extent2D {
    u32 width = 0;
    u32 height = 0;
    friend constexpr bool operator==(Extent2D a, Extent2D b) { return a.width == b.width && a.height == b.height; }
};

struct Rect2D {
    i32 x = 0, y = 0;  // top-left origin
    u32 width = 0, height = 0;
};

enum class TextureFormat : u16 {
    RGBA8_UNorm, RGBA8_SRGB, BGRA8_UNorm, RGBA16F, RGBA32F,
    R8_UNorm, RG8_UNorm, R11G11B10F,
    BC1_UNorm, BC3_UNorm, BC5_UNorm, BC7_UNorm, BC7_SRGB,  // cooked formats (FR-AST-050)
    D32_Float, D24_UNorm_S8, D16_UNorm,
};

bool is_depth_format(TextureFormat f);
bool is_compressed_format(TextureFormat f);
// Bytes per pixel of uncompressed formats (0 for block-compressed ones).
u32 bytes_per_pixel(TextureFormat f);

// Vertex attribute formats. The architecture sketch reuses TextureFormat for attributes; a separate
// enum keeps float2/float3 (which have no texture format) expressible.
enum class VertexFormat : u8 { Float1, Float2, Float3, Float4, UNorm8x4, UInt1 };
u32 vertex_format_size(VertexFormat f);

enum class BufferUsage : u16 { Vertex, Index, Uniform, Storage, Indirect, Staging, IndirectArgs };
enum class TextureUsage : u16 { Sampled, ColorTarget, DepthTarget, SampledColorTarget };

enum class Filter : u8 { Nearest, Linear };
enum class MipFilter : u8 { None, Nearest, Linear };
enum class AddressMode : u8 { Repeat, ClampToEdge, MirroredRepeat };

enum class IndexFormat : u8 { U16, U32 };
enum class LoadOp : u8 { Load, Clear, DontCare };
enum class StoreOp : u8 { Store, DontCare };

enum class CompareOp : u8 { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class BlendMode : u8 { Opaque, PremultipliedAlpha, Alpha, Additive };
enum class CullMode : u8 { None, Front, Back };
enum class FillMode : u8 { Solid, Wireframe };
enum class PrimitiveTopology : u8 { Triangles, Lines, LineStrip, Points };

struct ClearValue {
    f32 color[4] = {0, 0, 0, 1};
    f32 depth = 1.0f;
    u8 stencil = 0;
};

// ---- resources ---------------------------------------------------------------------------------

struct BufferDesc {
    BufferUsage usage = BufferUsage::Vertex;
    usize size_bytes = 0;
    const char* debug_name = "";
};

struct TextureDesc {
    Extent2D size;
    u16 mip_levels = 1;
    TextureFormat format = TextureFormat::RGBA8_UNorm;
    TextureUsage usage = TextureUsage::Sampled;
    const char* debug_name = "";
};

struct SamplerDesc {
    Filter min_filter = Filter::Linear;
    Filter mag_filter = Filter::Linear;
    MipFilter mip = MipFilter::None;
    AddressMode wrap_s = AddressMode::ClampToEdge;
    AddressMode wrap_t = AddressMode::ClampToEdge;
    f32 max_aniso = 1.0f;
};

// ---- pipelines ---------------------------------------------------------------------------------

// GLSL 450 core source for one stage. Until the asset pipeline lands (M2: ShaderKey resolved by
// assets, source cooked + validated) pipelines take source text directly. Binding numbers follow
// the fixed bank table (rhi-renderer.md §3.3): UBO 0 FrameU, 1 PassU, 2 MaterialU, samplers 6+.
// Separable programs: vertex shaders must redeclare `out gl_PerVertex { vec4 gl_Position; };`.
struct ShaderSource {
    StringView glsl;
    const char* debug_name = "";
};

struct VertexAttribute {
    u8 location = 0;
    VertexFormat format = VertexFormat::Float3;
    u16 offset = 0;
};

struct VertexLayout {
    Span<const VertexAttribute> attrs;
    u16 stride = 0;
};

struct DepthState {
    bool test = false;
    bool write = false;
    CompareOp compare = CompareOp::LessEqual;
};

struct BlendState {
    BlendMode mode = BlendMode::Opaque;
};

struct RasterState {
    CullMode cull = CullMode::None;
    FillMode fill = FillMode::Solid;
    bool front_ccw = true;
};

struct FixedState {
    DepthState depth;
    BlendState blend;
    RasterState raster;
    PrimitiveTopology topology = PrimitiveTopology::Triangles;
};

struct GraphicsPipelineDesc {
    ShaderSource vertex_shader;
    ShaderSource fragment_shader;
    VertexLayout vertex;
    FixedState state;
    const char* debug_name = "";
};

// ---- passes ------------------------------------------------------------------------------------

struct PassAttachment {
    TextureHandle texture;  // null = the window backbuffer (color) / default depth buffer (depth)
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    ClearValue clear;
};

struct RenderPassDesc {
    Span<const PassAttachment> colors;    // 1 at MVP; empty = depth-only pass
    std::optional<PassAttachment> depth;  // + stencil
    Rect2D viewport;                      // zero size = the whole target
    const char* debug_name = "";
};

// Fixed binding banks (rhi-renderer.md §3.3).
namespace bank {
inline constexpr u32 kFrame = 0;
inline constexpr u32 kPass = 1;
inline constexpr u32 kMaterial = 2;
inline constexpr u32 kObjects = 3;
inline constexpr u32 kFirstSampler = 6;
inline constexpr u32 kUniformBanks = 3;
}  // namespace bank

inline constexpr usize kUniformAlignment = 256;

}  // namespace mce::rhi
