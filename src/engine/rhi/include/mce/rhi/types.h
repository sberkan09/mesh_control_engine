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
    ASTC_4x4_UNorm, ASTC_4x4_SRGB,  // cooked for phones (M-MOBILE step 8); 16-byte 4x4 blocks
};

bool is_depth_format(TextureFormat f);
bool is_compressed_format(TextureFormat f);
// Bytes per pixel of uncompressed formats (0 for block-compressed ones).
u32 bytes_per_pixel(TextureFormat f);

// Vertex attribute formats. The architecture sketch reuses TextureFormat for attributes; a separate
// enum keeps float2/float3 (which have no texture format) expressible.
enum class VertexFormat : u8 {
    Float1, Float2, Float3, Float4, UNorm8x4, UInt1,
    SNorm16x2, SNorm16x4, UNorm16x2,  // quantized mesh streams (CHNK_MESH): read as normalized floats
};
u32 vertex_format_size(VertexFormat f);

enum class BufferUsage : u16 { Vertex, Index, Uniform, Storage, Indirect, Staging, IndirectArgs };
enum class TextureUsage : u16 {
    Sampled, ColorTarget, DepthTarget, SampledColorTarget,
    SampledStorage,  // read/write storage image for compute (bind_storage_image), also sampled (M-VISUAL 1)
};

// How a compute shader accesses a storage image (bind_storage_image).
enum class StorageAccess : u8 { Read, Write, ReadWrite };

// CommandBuffer::barrier bits: which later reads must see earlier compute (storage) writes.
namespace barrier {
inline constexpr u32 kStorageBuffer = 1u << 0;  // SSBO reads/writes in later shaders
inline constexpr u32 kStorageImage = 1u << 1;   // storage image reads/writes in later shaders
inline constexpr u32 kSampled = 1u << 2;        // texture() sampling of an image written as storage
inline constexpr u32 kIndirect = 1u << 3;       // draw/dispatch arguments written by a shader
inline constexpr u32 kVertexIndex = 1u << 4;    // vertex/index data written by a shader
inline constexpr u32 kUniform = 1u << 5;        // uniform data written by a shader
inline constexpr u32 kAll = 0xFFFFFFFFu;
}  // namespace barrier

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

enum class TextureDimension : u8 {
    Tex2D,
    Tex2DArray,  // GL_TEXTURE_2D_ARRAY: sprite-set pages, font atlases (rendering-2d D3); shaders use sampler2DArray
};

struct TextureDesc {
    Extent2D size;
    u16 mip_levels = 1;
    u16 array_layers = 1;  // > 1 requires Tex2DArray
    u8 samples = 1;        // > 1: a multisampled render target (Tex2D, one mip, never sampled; resolve() it)
    TextureDimension dimension = TextureDimension::Tex2D;
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
    // Depth-compare sampling (shadow maps): texture() returns the comparison result, filtered.
    bool compare = false;
    CompareOp compare_op = CompareOp::LessEqual;
};

// One mip level of one array layer (or a sub-rectangle of it) for RenderDevice::update_texture.
// Rows are tightly packed; row 0 is sampled at v = 0 (same convention as create_texture).
struct TextureRegion {
    u32 mip = 0;
    u32 layer = 0;
    u32 x = 0, y = 0;
    u32 width = 0, height = 0;  // zero = the whole mip level
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

// One compute stage (GLSL 450 core, `layout(local_size_x = ...) in;`). Storage images use the image
// units of bind_storage_image; SSBOs and uniform banks use the same bindings as graphics pipelines.
struct ComputePipelineDesc {
    ShaderSource compute_shader;
    const char* debug_name = "";
};

// ---- passes ------------------------------------------------------------------------------------

struct PassAttachment {
    TextureHandle texture;  // null = the window backbuffer (color) / default depth buffer (depth)
    u16 layer = 0;          // array layer of a Tex2DArray target (cascaded shadow maps); 0 otherwise
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

// glMultiDrawElementsIndirect / vkCmdDrawIndexedIndirect argument layout (rhi-renderer.md §3.2).
struct DrawIndexedIndirectCommand {
    u32 index_count;
    u32 instance_count;
    u32 first_index;
    i32 base_vertex;
    u32 base_instance;
};
static_assert(sizeof(DrawIndexedIndirectCommand) == 20);

// glDispatchComputeIndirect / vkCmdDispatchIndirect argument layout (work-group counts).
struct DispatchIndirectCommand {
    u32 groups_x;
    u32 groups_y;
    u32 groups_z;
};
static_assert(sizeof(DispatchIndirectCommand) == 12);

// Fixed binding banks (rhi-renderer.md §3.3).
namespace bank {
inline constexpr u32 kFrame = 0;
inline constexpr u32 kPass = 1;
inline constexpr u32 kMaterial = 2;
inline constexpr u32 kObjects = 3;
inline constexpr u32 kFirstSampler = 6;
inline constexpr u32 kAtlasArray = 6;  // 2D sprite-set array texture
inline constexpr u32 kObject = 3;      // per-draw ObjectU uniform block (3D, MVP); the Objects SSBO shares the number
inline constexpr u32 kAlbedo = 6;      // 3D base color (2D array, layer 0); shares the slot with the atlas
inline constexpr u32 kShadowMap = 9;   // sampler2DArrayShadow: the CSM cascades (M5a)
inline constexpr u32 kNormal = 7;      // 3D tangent-space normal map
inline constexpr u32 kOrm = 8;         // 3D occlusion/roughness/metallic (glTF metallicRoughness + packed AO)
inline constexpr u32 kSky = 10;        // equirect sky texture (2D array, layer 0)
inline constexpr u32 kFont = 11;
inline constexpr u32 kEmissive = 13;   // 3D emissive map (the spare slot of the §3.3 table)
inline constexpr u32 kIblEnv = 12;     // 3D image-based lighting: GGX pre-filtered sky (2D array, M-VISUAL 2)
inline constexpr u32 kIblBrdfLut = 14; // 3D image-based lighting: split-sum BRDF LUT
inline constexpr u32 kIblSh = 15;      // 3D image-based lighting: SH9 irradiance (9×1)
// std430 storage bindings (their own namespace in GL, independent of the sampler units above).
inline constexpr u32 kClusters = 4;     // Forward+ per-cluster {offset, count}
inline constexpr u32 kLightPool = 5;    // point lights
inline constexpr u32 kLightIndices = 6; // the clusters' light index list
inline constexpr u32 kJointPalette = 7; // skinned meshes' joint matrices (vertex stage, FR-R3D-060)
inline constexpr u32 kClusterCull = 8;    // GPU light assignment input: culled view-space lights (compute)
inline constexpr u32 kClusterBounds = 9;  // GPU light assignment input: tile extents + slice edges (compute)
inline constexpr u32 kClusterCounter = 10; // GPU light assignment: index-list bump counter + drops (compute)
inline constexpr u32 kUniformBanks = 4;  // FrameU, PassU, MaterialU, ObjectU
}  // namespace bank

inline constexpr usize kUniformAlignment = 256;
inline constexpr u32 kMaxTimestamps = 32;  // GPU timestamp queries per frame slot (CommandBuffer::write_timestamp)

}  // namespace mce::rhi
