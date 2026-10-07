// GUIDs, XXH64 reference vectors, chunk payload formats (round trip + bounds checks).

#include <doctest/doctest.h>

#include <cstring>

#include "mce/asset/formats.h"
#include "mce/asset/guid.h"
#include "mce/asset/handle.h"
#include "mce/asset/xxhash.h"

using namespace mce;
using namespace mce::asset;

TEST_CASE("guid: canonical parse/format round trip") {
    auto g = parse_guid("0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0");
    REQUIRE(g);
    CHECK(g.value().hi == 0x0f1e2d3c4b5a6978ull);
    CHECK(g.value().lo == 0x8796a5b4c3d2e1f0ull);
    CHECK(to_string(g.value()) == "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0");
    CHECK(to_hex(g.value()) == "0f1e2d3c4b5a69788796a5b4c3d2e1f0");
    CHECK(parse_guid("0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0").value() == g.value());
}

TEST_CASE("guid: malformed text is a ParseError") {
    for (const char* bad : {"", "0f1e2d3c4b5a69788796a5b4c3d2e1f0", "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f", "0f1e2d3c_4b5a-6978-8796-a5b4c3d2e1f0",
                            "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1fg"}) {
        auto r = parse_guid(bad);
        REQUIRE(!r);
        CHECK(r.error().code == Errc::ParseError);
    }
}

TEST_CASE("guid: ordering is by (hi, lo)") {
    CHECK(Guid{1, 9} < Guid{2, 0});
    CHECK(Guid{1, 1} < Guid{1, 2});
    CHECK(Guid{} == Guid{});
    CHECK(Guid{}.is_null());
}

TEST_CASE("xxh64: reference vectors") {
    CHECK(xxh64("", 0, 0) == 0xEF46DB3751D8E999ull);
    CHECK(xxh64(StringView("abc")) == 0x44BC2CF5AD770999ull);
    // Exercises the 32-byte stripe loop and every tail path.
    const char* s = "Nobody inspects the spammish repetition";
    CHECK(xxh64(StringView(s)) == 0xFBCEA83C8A378BF1ull);
    CHECK(xxh64(StringView("abc"), 1) != xxh64(StringView("abc"), 0));
}

TEST_CASE("formats: texture write/read and pixel views") {
    u8 mip0[4 * 2 * 4], mip1[1 * 1 * 4];
    for (usize i = 0; i < sizeof(mip0); ++i) mip0[i] = static_cast<u8>(i);
    std::memset(mip1, 0xAB, sizeof(mip1));
    const TextureMipData mips[2] = {{4, 2, Span<const byte>(reinterpret_cast<const byte*>(mip0), sizeof(mip0))},
                                    {1, 1, Span<const byte>(reinterpret_cast<const byte*>(mip1), sizeof(mip1))}};
    Array<byte> payload;
    write_texture(TextureFormat::RGBA8, 1, TextureFlags::Srgb, Span<const TextureMipData>(mips, 2), payload);
    auto v = TextureView::read(payload.span());
    REQUIRE(v);
    CHECK(v.value().header->width == 4);
    CHECK(v.value().header->height == 2);
    CHECK(v.value().header->mip_count == 2);
    CHECK(v.value().header->flags == TextureFlags::Srgb);
    CHECK(std::memcmp(v.value().pixels(0).data(), mip0, sizeof(mip0)) == 0);
    CHECK(std::memcmp(v.value().pixels(1).data(), mip1, sizeof(mip1)) == 0);
    for (const TextureMip& m : v.value().mips) CHECK(m.offset % 16 == 0);

    // Corrupt the mip table: bounds-checked rejection, never an out-of-range view.
    reinterpret_cast<TextureMip*>(payload.data() + sizeof(TextureHeader))[1].offset = payload.size();
    CHECK(!TextureView::read(payload.span()));
    CHECK(!TextureView::read(payload.span().first(8)));
}

TEST_CASE("formats: sfx write/read with loop validation") {
    const f32 s[6] = {0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f};
    SfxHeader h{};
    h.sample_rate = 48000;
    h.channels = 2;
    h.frame_count = 3;
    h.flags = SfxFlags::Looping;
    h.loop_start = 1;
    h.loop_end = 3;
    Array<byte> payload;
    write_sfx(h, Span<const f32>(s, 6), payload);
    auto v = SfxView::read(payload.span());
    REQUIRE(v);
    CHECK(v.value().samples.size() == 6);
    CHECK(v.value().samples[5] == doctest::Approx(-0.3f));
    reinterpret_cast<SfxHeader*>(payload.data())->loop_end = 4;  // past the end
    CHECK(!SfxView::read(payload.span()));
}

TEST_CASE("formats: font lookup and kerning") {
    FontHeader h{};
    h.pixel_height = 13;
    h.atlas_width = 4;
    h.atlas_height = 4;
    const FontGlyph glyphs[2] = {{65, 0, 0, 2, 2, 0, -2, 2, 0, 3}, {66, 2, 0, 4, 2, 0, -2, 2, 0, 3}};
    const FontKern kerns[1] = {{65, 66, -1.5f}};
    byte atlas[16] = {};
    Array<byte> payload;
    write_font(h, Span<const FontGlyph>(glyphs, 2), Span<const FontKern>(kerns, 1), Span<const byte>(atlas, 16), payload);
    auto v = FontView::read(payload.span());
    REQUIRE(v);
    REQUIRE(v.value().find(66));
    CHECK(v.value().find(66)->x0 == 2);
    CHECK(v.value().find(67) == nullptr);
    CHECK(v.value().kern(65, 66) == -1.5f);
    CHECK(v.value().kern(66, 65) == 0.0f);
}

TEST_CASE("formats: manifest sorts by GUID and binary-searches") {
    ManifestItem items[3] = {{Guid{3, 0}, 30, ChunkType::Texture, "c.png"},
                             {Guid{1, 0}, 10, ChunkType::Sfx, "a.wav"},
                             {Guid{2, 0}, 20, ChunkType::Scene, "b.mcscene"}};
    Array<byte> payload;
    write_manifest(Span<ManifestItem>(items, 3), payload);
    auto m = ManifestView::read(payload.span());
    REQUIRE(m);
    REQUIRE(m.value().size() == 3);
    CHECK(m.value().entry(0).guid() == Guid{1, 0});
    CHECK(m.value().path(0) == "a.wav");
    CHECK(m.value().entry(2).content_hash == 30);
    CHECK(m.value().find(Guid{2, 0}) == 1);
    CHECK(m.value().find(Guid{9, 0}) == 3);
    // Out-of-order entries are rejected (the sort is part of the format).
    std::swap(reinterpret_cast<ManifestEntry*>(payload.data() + sizeof(ManifestHeader))[0],
              reinterpret_cast<ManifestEntry*>(payload.data() + sizeof(ManifestHeader))[1]);
    CHECK(!ManifestView::read(payload.span()));
}

TEST_CASE("handle: AssetRef is reflected as FieldKind::AssetRef") {
    using Traits = refl::FieldTraits<AssetRef<Texture>>;
    static_assert(Traits::supported);
    static_assert(Traits::kind == refl::FieldKind::AssetRef);
    CHECK(StringView(Traits::type_name) == "Texture");
    ChunkType t;
    CHECK(chunk_type_for_asset_name("Font", t));
    CHECK(t == ChunkType::Font);
    CHECK(!chunk_type_for_asset_name("Nope", t));
}
