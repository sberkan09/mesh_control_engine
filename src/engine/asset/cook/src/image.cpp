// PNG decode / encode through stb (the stb wall keeps them inside the cook).

#include "mce/cook/image.h"

#include <cstring>

#include "stb_image.h"
#include "stb_image_write.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.image";

void append_bytes(void* ctx, void* data, int size) {
    auto& out = *static_cast<Array<byte>*>(ctx);
    const usize at = out.size();
    out.resize(at + usize(size));
    std::memcpy(out.data() + at, data, usize(size));
}

}  // namespace

bool image_size(Span<const byte> bytes, u32& width, u32& height) {
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &comp)) return false;
    width = static_cast<u32>(w);
    height = static_cast<u32>(h);
    return true;
}

Result<Image> decode_png(Span<const byte> bytes) {
    if (bytes.size() > 0x7FFFFFFF) return Error(Errc::ImportFailed, kStage, "image too large");
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (!px) return Error(Errc::ImportFailed, kStage, stbi_failure_reason());
    Image img;
    img.width = u32(w);
    img.height = u32(h);
    img.rgba.resize(usize(w) * usize(h) * 4);
    std::memcpy(img.rgba.data(), px, img.rgba.size());
    stbi_image_free(px);
    return img;
}

Result<Image16> decode_png_grey16(Span<const byte> bytes) {
    if (bytes.size() > 0x7FFFFFFF) return Error(Errc::ImportFailed, kStage, "image too large");
    int w = 0, h = 0, comp = 0;
    stbi_us* px = stbi_load_16_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &comp, 1);
    if (!px) return Error(Errc::ImportFailed, kStage, stbi_failure_reason());
    Image16 img;
    img.width = u32(w);
    img.height = u32(h);
    img.grey.resize(usize(w) * usize(h));
    std::memcpy(img.grey.data(), px, img.grey.size() * sizeof(u16));
    stbi_image_free(px);
    return img;
}

Result<void> encode_png(u32 width, u32 height, Span<const u8> rgba, Array<byte>& out) {
    if (width == 0 || height == 0 || rgba.size() != usize(width) * height * 4)
        return Error(Errc::InvalidArgument, kStage, "rgba is not width * height * 4 bytes");
    if (!stbi_write_png_to_func(append_bytes, &out, int(width), int(height), 4, rgba.data(), int(width * 4)))
        return Error(Errc::IoError, kStage, "png encode failed");
    return ok();
}

}  // namespace mce::cook
