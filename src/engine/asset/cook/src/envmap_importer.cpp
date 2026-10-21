// .mcenv -> CHNK_TEX: a baked environment (envmap.h) as an RGBA8 2D array, stored as authored
// (never ASTC: the RGBM alpha carries the exponent, and block compression would smear it).

#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/cook/envmap.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.envmap";

u32 read_u32(const byte* p) {
    u32 v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<u32>(p[i]);
    return v;
}

class EnvmapImporter final : public IImporter {
public:
    StringView id() const override { return "envmap"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Texture; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".mcenv"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        if (!detail::check_keys(in.settings, {}, in.display_path, kStage, diags)) return false;
        const Span<const byte> src = in.source;
        if (src.size() < kEnvHeaderBytes || std::memcmp(src.data(), kEnvMagic, sizeof(kEnvMagic)) != 0) {
            diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, "not a .mcenv file (bad magic)");
            return false;
        }
        const u32 w = read_u32(src.data() + 8), h = read_u32(src.data() + 12), layers = read_u32(src.data() + 16),
                  encoding = read_u32(src.data() + 20);
        if (encoding != kEnvEncodingRgbm) {
            diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, "unknown .mcenv encoding");
            return false;
        }
        if (w == 0 || h == 0 || layers == 0 || w > 4096 || h > 4096 || layers > 64) {
            diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, ".mcenv dimensions out of range");
            return false;
        }
        const u64 bytes = u64(w) * h * 4 * layers;
        if (src.size() != kEnvHeaderBytes + bytes) {
            diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, ".mcenv size does not match its header");
            return false;
        }
        const asset::TextureMipData mip{w, h, src.subspan(kEnvHeaderBytes, static_cast<usize>(bytes))};
        Array<byte> payload;
        asset::write_texture(asset::TextureFormat::RGBA8, layers, 0, Span<const asset::TextureMipData>(&mip, 1), payload);
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Texture;
        d.version = asset::chunk_version(asset::TEXTURE_VERSION, 0);
        d.user_flags = static_cast<u32>(asset::TextureFormat::RGBA8);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

void write_envmap(u32 width, u32 height, u32 layers, Span<const u8> rgba, Array<byte>& out) {
    MCE_ASSERT(rgba.size() == usize(width) * height * 4 * layers);
    for (char c : kEnvMagic) out.push_back(static_cast<byte>(c));
    for (u32 v : {width, height, layers, kEnvEncodingRgbm})
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<byte>((v >> (8 * i)) & 0xFFu));
    for (u8 b : rgba) out.push_back(static_cast<byte>(b));
}

mem::UniquePtr<IImporter> make_envmap_importer() { return mem::make_unique<EnvmapImporter>(mem::heap()); }

}  // namespace mce::cook
