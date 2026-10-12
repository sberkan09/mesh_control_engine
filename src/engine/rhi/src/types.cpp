#include "mce/rhi/types.h"

namespace mce::rhi {

bool is_depth_format(TextureFormat f) {
    return f == TextureFormat::D32_Float || f == TextureFormat::D24_UNorm_S8 || f == TextureFormat::D16_UNorm;
}

bool is_compressed_format(TextureFormat f) {
    switch (f) {
        case TextureFormat::BC1_UNorm:
        case TextureFormat::BC3_UNorm:
        case TextureFormat::BC5_UNorm:
        case TextureFormat::BC7_UNorm:
        case TextureFormat::BC7_SRGB:
        case TextureFormat::ASTC_4x4_UNorm:
        case TextureFormat::ASTC_4x4_SRGB: return true;
        default: return false;
    }
}

u32 bytes_per_pixel(TextureFormat f) {
    switch (f) {
        case TextureFormat::RGBA8_UNorm:
        case TextureFormat::RGBA8_SRGB:
        case TextureFormat::BGRA8_UNorm:
        case TextureFormat::R11G11B10F:
        case TextureFormat::D32_Float:
        case TextureFormat::D24_UNorm_S8: return 4;
        case TextureFormat::RGBA16F: return 8;
        case TextureFormat::RGBA32F: return 16;
        case TextureFormat::R8_UNorm: return 1;
        case TextureFormat::RG8_UNorm:
        case TextureFormat::D16_UNorm: return 2;
        default: return 0;
    }
}

u32 vertex_format_size(VertexFormat f) {
    switch (f) {
        case VertexFormat::Float1: return 4;
        case VertexFormat::Float2: return 8;
        case VertexFormat::Float3: return 12;
        case VertexFormat::Float4: return 16;
        case VertexFormat::UNorm8x4: return 4;
        case VertexFormat::UInt1: return 4;
        case VertexFormat::SNorm16x2: return 4;
        case VertexFormat::SNorm16x4: return 8;
        case VertexFormat::UNorm16x2: return 4;
    }
    return 0;
}

}  // namespace mce::rhi
