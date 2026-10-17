// M-CONTENT 1b-iii, headless: rhi_format_for, the DeviceCaps refusal in create_texture (against a stub device whose
// caps the test sets), and compose_layer_array's BC4 -> BC5 case. The GPU side (upload, sample, read back, the
// unpack_normal shading) is tests/render_gl/test_render_bc_gl.cpp.

#include <doctest/doctest.h>

#include <cstring>

#include "mce/asset/formats.h"
#include "mce/log.h"
#include "mce/render/asset_upload.h"
#include "mce/render/terrain.h"
#include "mce/render/texture_store.h"
#include "mce/rhi/rhi.h"

using namespace mce;
using namespace mce::render;
using AF = asset::TextureFormat;
using RF = rhi::TextureFormat;

namespace {

// Everything a TextureStore asks of a device, with settable caps and a record of the textures created.
class StubDevice final : public rhi::RenderDevice {
public:
    rhi::BufferHandle create_buffer(const rhi::BufferDesc&, Span<const byte> = {}) override { return {1, 1}; }
    void update_buffer(rhi::BufferHandle, u64, Span<const byte>) override {}
    rhi::TextureHandle create_texture(const rhi::TextureDesc& desc, Span<const byte>) override {
        ++creates;
        if (null_textures) return {};
        last = desc;
        return {++next, 1};
    }
    void update_texture(rhi::TextureHandle, const rhi::TextureRegion&, Span<const byte>) override {}
    rhi::SamplerHandle create_sampler(const rhi::SamplerDesc&) override { return {1, 1}; }
    Result<rhi::PipelineHandle> create_pipeline(const rhi::GraphicsPipelineDesc&) override { return rhi::PipelineHandle{1, 1}; }
    void destroy_buffer(rhi::BufferHandle) override {}
    void destroy_texture(rhi::TextureHandle) override {}
    void destroy_sampler(rhi::SamplerHandle) override {}
    void destroy_pipeline(rhi::PipelineHandle) override {}
    bool is_alive(rhi::BufferHandle) const override { return true; }
    bool is_alive(rhi::TextureHandle) const override { return true; }
    bool is_alive(rhi::PipelineHandle) const override { return true; }
    rhi::Extent2D texture_size(rhi::TextureHandle) const override { return {}; }
    rhi::TextureDesc texture_desc(rhi::TextureHandle) const override { return last; }
    void begin_frame(u64) override {}
    void submit(const rhi::CommandBuffer&) override {}
    void end_frame() override {}
    void present(bool) override {}
    u64 frame_index() const override { return 0; }
    rhi::Extent2D backbuffer_size() const override { return {}; }
    ResultV read_pixels(rhi::TextureHandle, rhi::Rect2D, Span<u8>) override { return ok(); }
    const rhi::DeviceCaps& caps() const override { return caps_; }
    const rhi::FrameStats& last_frame_stats() const override { return stats_; }
    const rhi::GpuTimestamps& last_gpu_timestamps() const override { return ts_; }
    usize texture_memory_bytes() const override { return 0; }
    u32 validation_error_count() const override { return 0; }
    mem::FrameSlotFence* fence_for_frame(u64) override { return nullptr; }

    rhi::DeviceCaps caps_;
    rhi::FrameStats stats_;
    rhi::GpuTimestamps ts_;
    rhi::TextureDesc last;
    u32 creates = 0;
    u32 next = 10;
    bool null_textures = false;
};

// A cooked texture of `size`² with `mips` levels: every block filled with `fill`, `block` bytes per 4x4 block.
void bc_blob(AF format, u32 flags, u32 size, u32 mips, u32 block, byte fill, Array<byte>& out) {
    Array<byte> px[8];
    asset::TextureMipData data[8];
    for (u32 m = 0; m < mips; ++m) {
        const u32 s = size >> m;
        px[m].resize(usize((s + 3) / 4) * ((s + 3) / 4) * block, fill);
        data[m] = {s, s, px[m].span()};
    }
    asset::write_texture(format, 1, flags, Span<const asset::TextureMipData>(data, mips), out);
}

}  // namespace

TEST_CASE("bc runtime: rhi_format_for maps every cooked format and the sRGB flag") {
    CHECK(rhi_format_for(AF::RGBA8, false) == RF::RGBA8_UNorm);
    CHECK(rhi_format_for(AF::RGBA8, true) == RF::RGBA8_SRGB);
    CHECK(rhi_format_for(AF::ASTC_4x4, false) == RF::ASTC_4x4_UNorm);
    CHECK(rhi_format_for(AF::ASTC_4x4, true) == RF::ASTC_4x4_SRGB);
    CHECK(rhi_format_for(AF::RGBA16F, false) == RF::RGBA16F);
    CHECK(rhi_format_for(AF::BC1, false) == RF::BC1_UNorm);
    CHECK(rhi_format_for(AF::BC1, true) == RF::BC1_SRGB);
    CHECK(rhi_format_for(AF::BC4, false) == RF::BC4_UNorm);
    CHECK(rhi_format_for(AF::BC5, false) == RF::BC5_UNorm);
    CHECK(rhi_format_for(AF::BC7, false) == RF::BC7_UNorm);
    CHECK(rhi_format_for(AF::BC7, true) == RF::BC7_SRGB);
    // Block sizes agree between the asset and the RHI side.
    for (const AF f : {AF::BC1, AF::BC4, AF::BC5, AF::BC7})
        CHECK(asset::texture_level_bytes(f, 8, 8) == rhi::level_bytes(rhi_format_for(f, false), 8, 8));
}

TEST_CASE("bc runtime: upload_cooked_texture reaches the device with the BC format") {
    StubDevice dev;
    dev.caps_.texture_bc = true;
    dev.caps_.texture_bc_srgb = true;
    TextureStore store;
    REQUIRE(store.init(dev).has_value());
    struct Case {
        AF format;
        u32 flags;
        u32 block;
        RF expect;
    };
    const Case cases[] = {{AF::BC1, 0, 8, RF::BC1_UNorm},
                          {AF::BC1, asset::TextureFlags::Srgb, 8, RF::BC1_SRGB},
                          {AF::BC4, 0, 8, RF::BC4_UNorm},
                          {AF::BC5, 0, 16, RF::BC5_UNorm},
                          {AF::BC7, 0, 16, RF::BC7_UNorm},
                          {AF::BC7, asset::TextureFlags::Srgb, 16, RF::BC7_SRGB}};
    u32 key = 1;
    for (const Case& c : cases) {
        Array<byte> blob;
        bc_blob(c.format, c.flags, 16, 5, c.block, byte{0}, blob);  // 16, 8, 4, 2, 1: the last two are padded blocks
        auto tex = upload_cooked_texture(store, key++, 1, blob.span());
        REQUIRE(tex.has_value());
        CHECK(dev.last.format == c.expect);
        CHECK(dev.last.mip_levels == 5);
    }
}

TEST_CASE("bc runtime: a device that cannot sample the format refuses it, and the texture draws as the error checker") {
    StubDevice dev;
    TextureStore store;
    REQUIRE(store.init(dev).has_value());  // RGBA8 white and checker need no cap
    const u32 base = dev.creates;
    Array<byte> bc7, astc, rgba;
    bc_blob(AF::BC7, 0, 8, 1, 16, byte{0}, bc7);
    bc_blob(AF::ASTC_4x4, 0, 8, 1, 16, byte{0}, astc);
    rgba.resize(8 * 8 * 4, byte{0});

    // Neither cap: BC and ASTC are refused before the device is touched.
    auto r = upload_cooked_texture(store, 1, 1, bc7.span());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == Errc::Unsupported);
    CHECK(StringView(r.error().stage) == "render.texture.format");
    CHECK_FALSE(upload_cooked_texture(store, 2, 1, astc.span()).has_value());
    CHECK(dev.creates == base);
    CHECK_FALSE(store.contains(1));
    CHECK(store.get(1) == store.get(0x7777));  // the error checker

    // The caps are per family: BC does not unlock ASTC.
    dev.caps_.texture_bc = true;
    CHECK(upload_cooked_texture(store, 1, 1, bc7.span()).has_value());
    CHECK_FALSE(upload_cooked_texture(store, 2, 1, astc.span()).has_value());
    dev.caps_.texture_astc = true;
    CHECK(upload_cooked_texture(store, 2, 1, astc.span()).has_value());

    // The same refusal reaches raw uploads (compose_layer_array's arrays, the terrain layers).
    dev.caps_.texture_bc = false;
    const TextureMipUpload mip{8, 8, bc7.span().subspan(0, 0)};
    TextureUpload up;
    up.format = RF::BC5_UNorm;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    auto raw = create_texture(dev, up);
    REQUIRE_FALSE(raw.has_value());
    CHECK(raw.error().code == Errc::Unsupported);
}

TEST_CASE("bc runtime: BC1_SRGB is refused when the device has BC but no sRGB BC1") {
    StubDevice dev;
    dev.caps_.texture_bc = true;
    dev.caps_.texture_bc_srgb = false;
    TextureStore store;
    REQUIRE(store.init(dev).has_value());
    const u32 base = dev.creates;
    Array<byte> lin, srgb, bc7srgb;
    bc_blob(AF::BC1, 0, 8, 1, 8, byte{0}, lin);
    bc_blob(AF::BC1, asset::TextureFlags::Srgb, 8, 1, 8, byte{0}, srgb);
    bc_blob(AF::BC7, asset::TextureFlags::Srgb, 8, 1, 16, byte{0}, bc7srgb);
    CHECK(upload_cooked_texture(store, 1, 1, lin.span()).has_value());
    CHECK(dev.last.format == RF::BC1_UNorm);
    const u32 after_lin = dev.creates;
    auto r = upload_cooked_texture(store, 2, 1, srgb.span());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == Errc::Unsupported);
    CHECK(StringView(r.error().stage) == "render.texture.format");
    CHECK(dev.creates == after_lin);  // refused before the device is touched
    CHECK(after_lin == base + 1);
    CHECK(upload_cooked_texture(store, 3, 1, bc7srgb.span()).has_value());  // BC7 sRGB is gated by texture_bc only
    CHECK(dev.last.format == RF::BC7_SRGB);
    dev.caps_.texture_bc_srgb = true;
    CHECK(upload_cooked_texture(store, 2, 1, srgb.span()).has_value());
    CHECK(dev.last.format == RF::BC1_SRGB);
}

TEST_CASE("bc runtime: create_texture reports a null handle from the backend") {
    StubDevice dev;
    dev.caps_.texture_bc = true;
    Array<byte> px;
    px.resize(64, byte{0});
    const TextureMipUpload mip{8, 8, px.span()};
    TextureUpload up;
    up.format = RF::BC7_UNorm;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    CHECK(create_texture(dev, up).has_value());
    dev.null_textures = true;
    auto r = create_texture(dev, up);
    REQUIRE_FALSE(r.has_value());
    CHECK(StringView(r.error().stage) == "render.texture.create");
}

TEST_CASE("bc runtime: compose_layer_array widens a BC4 height into a BC5 normal array") {
    // Layer 0: BC5 normal, every block 0xA0; layer 1: BC4 height, every block 0x55. 8x8 with 2 mips.
    Array<byte> normal, height;
    bc_blob(AF::BC5, 0, 8, 2, 16, byte{0xA0}, normal);
    bc_blob(AF::BC4, 0, 8, 2, 8, byte{0x55}, height);
    const asset::TextureView vn = asset::TextureView::read(normal.span()).value(), vh = asset::TextureView::read(height.span()).value();

    const asset::TextureView* both[2] = {&vn, &vh};
    LayerArray arr;
    REQUIRE(compose_layer_array(Span<const asset::TextureView* const>(both, 2), arr, "t", true));
    CHECK(arr.present == 0b11u);
    CHECK(arr.upload.format == RF::BC5_UNorm);
    REQUIRE(arr.upload.mips.size() == 2);
    const usize layer0 = 4 * 16;  // 8x8 = 2x2 blocks of 16 bytes
    REQUIRE(arr.upload.mips[0].pixels.size() == 2 * layer0);
    for (usize k = 0; k < 4; ++k) {
        for (usize i = 0; i < 16; ++i) CHECK(u8(arr.upload.mips[0].pixels[k * 16 + i]) == 0xA0);
        for (usize i = 0; i < 8; ++i) CHECK(u8(arr.upload.mips[0].pixels[layer0 + k * 16 + i]) == 0x55);  // the bc4 block
        for (usize i = 8; i < 16; ++i) CHECK(u8(arr.upload.mips[0].pixels[layer0 + k * 16 + i]) == 0);   // G = 0
    }
    REQUIRE(arr.upload.mips[1].pixels.size() == 2 * 16);  // 4x4: one block per layer
    CHECK(u8(arr.upload.mips[1].pixels[16]) == 0x55);
    CHECK(u8(arr.upload.mips[1].pixels[16 + 8]) == 0);

    // The BC4 layer may come first: the array is still BC5.
    const asset::TextureView* swapped[2] = {&vh, &vn};
    REQUIRE(compose_layer_array(Span<const asset::TextureView* const>(swapped, 2), arr, "t", true));
    CHECK(arr.present == 0b11u);
    CHECK(arr.upload.format == RF::BC5_UNorm);
    CHECK(u8(arr.upload.mips[0].pixels[0]) == 0x55);
    CHECK(u8(arr.upload.mips[0].pixels[8]) == 0);
    CHECK(u8(arr.upload.mips[0].pixels[layer0]) == 0xA0);

    // BC4 alone stays BC4 and copies as it is.
    const asset::TextureView* only[2] = {&vh, nullptr};
    REQUIRE(compose_layer_array(Span<const asset::TextureView* const>(only, 2), arr));
    CHECK(arr.upload.format == RF::BC4_UNorm);
    CHECK(arr.upload.mips[0].pixels.size() == 2 * 4 * 8);
    CHECK(u8(arr.upload.mips[0].pixels[7]) == 0x55);

    // Any other mix stays refused: a BC4 beside RGBA8 is left out with a warning.
    Array<byte> rgba;
    Array<byte> px;
    px.resize(8 * 8 * 4, byte{1});
    const asset::TextureMipData md[1] = {{8, 8, px.span()}};
    asset::write_texture(AF::RGBA8, 1, 0, Span<const asset::TextureMipData>(md, 1), rgba);
    const asset::TextureView vr = asset::TextureView::read(rgba.span()).value();
    const asset::TextureView* mixed[2] = {&vr, &vh};
    REQUIRE(compose_layer_array(Span<const asset::TextureView* const>(mixed, 2), arr));
    CHECK(arr.present == 0b01u);
    CHECK(arr.upload.format == RF::RGBA8_UNorm);
}

TEST_CASE("bc runtime: widening is opt-in, and only layers that pass validation make the array BC5") {
    Array<byte> normal, height, big_normal;
    bc_blob(AF::BC5, 0, 8, 2, 16, byte{0xA0}, normal);
    bc_blob(AF::BC4, 0, 8, 2, 8, byte{0x55}, height);
    bc_blob(AF::BC5, 0, 16, 3, 16, byte{0xB0}, big_normal);  // a different size: fails validation against the first layer
    const asset::TextureView vn = asset::TextureView::read(normal.span()).value(), vh = asset::TextureView::read(height.span()).value(),
                             vb = asset::TextureView::read(big_normal.span()).value();
    LayerArray arr;

    // Default: a BC4 beside a BC5 is the old refusal (the second layer is left out), not a widening.
    const asset::TextureView* both[2] = {&vn, &vh};
    REQUIRE(compose_layer_array(Span<const asset::TextureView* const>(both, 2), arr));
    CHECK(arr.present == 0b01u);
    CHECK(arr.upload.format == RF::BC5_UNorm);
    const asset::TextureView* bc4_first[2] = {&vh, &vn};
    REQUIRE(compose_layer_array(Span<const asset::TextureView* const>(bc4_first, 2), arr));
    CHECK(arr.present == 0b01u);
    CHECK(arr.upload.format == RF::BC4_UNorm);

    // Opt-in, but the BC5 layer is a different size than the BC4 first layer: it fails validation, so the array stays BC4.
    const asset::TextureView* mismatch[2] = {&vh, &vb};
    REQUIRE(compose_layer_array(Span<const asset::TextureView* const>(mismatch, 2), arr, "t", true));
    CHECK(arr.present == 0b01u);
    CHECK(arr.upload.format == RF::BC4_UNorm);
}

TEST_CASE("bc runtime: the unsupported-format refusal is logged once, with no per-texture upload warning") {
    StubDevice dev;  // no BC cap
    TextureStore store;
    REQUIRE(store.init(dev).has_value());
    Array<byte> blob;
    bc_blob(AF::BC7, 0, 8, 1, 16, byte{0}, blob);
    log::set_level(log::Level::Info);
    const u64 seq = log::console_last_seq();
    for (u32 k = 1; k <= 3; ++k) CHECK_FALSE(upload_cooked_texture(store, k, 1, blob.span()).has_value());
    log::ConsoleLine lines[32];
    const usize n = log::read_console(seq, Span<log::ConsoleLine>(lines, 32));
    log::set_level(log::Level::Warn);
    for (usize i = 0; i < n; ++i)
        CHECK(StringView(lines[i].text.c_str()).find("texture upload failed") == StringView::npos);
}
