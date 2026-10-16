// Data mods (ADR-0034): the ModInfo chunk, mod ids, resolve_mods (which packs load, in what order)
// and the AssetSystem overlay a mod relies on (earlier mounts win, but only with the same type).

#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "mce/asset/asset_system.h"
#include "mce/asset/mods.h"
#include "mce/asset/xxhash.h"
#include "test_support.h"

using namespace mce;
using namespace mce::asset;
namespace fs = std::filesystem;

namespace {

const Guid kTex{0x0102030405060708ull, 0x1112131415161718ull};
const Guid kNew{0x3102030405060708ull, 0x3112131415161718ull};

std::string tmp_dir(const char* name) {
    fs::path p = fs::path(MCE_TEST_TMP_DIR) / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p.generic_string();
}

void write_bytes(const std::string& path, Span<const byte> b) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f);
    std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
}

void texture_payload(u8 shade, Array<byte>& out) {
    u8 px[2 * 2 * 4];
    std::memset(px, shade, sizeof(px));
    const TextureMipData mip{2, 2, Span<const byte>(reinterpret_cast<const byte*>(px), sizeof(px))};
    write_texture(TextureFormat::RGBA8, 1, 0, Span<const TextureMipData>(&mip, 1), out);
}

void sfx_payload(Array<byte>& out) {
    const f32 s[2] = {0, 0.5f};
    SfxHeader h{};
    h.sample_rate = 48000;
    h.channels = 1;
    h.frame_count = 2;
    write_sfx(h, Span<const f32>(s, 2), out);
}

struct PackAsset {
    Guid guid;
    ChunkType type;
    u8 shade;  // textures only
};

// A pack: manifest, an optional ModInfo chunk, then one chunk per asset (as the cook lays it out).
std::string make_pack(const std::string& path, std::vector<PackAsset> assets, const ModInfoData* info) {
    std::vector<Array<byte>> payloads(assets.size());
    std::vector<ManifestItem> items;
    for (usize i = 0; i < assets.size(); ++i) {
        if (assets[i].type == ChunkType::Texture) texture_payload(assets[i].shade, payloads[i]);
        else if (assets[i].type == ChunkType::Script) {
            const byte code[4] = {byte(1), byte(2), byte(3), byte(4)};  // not valid Luau: the mount must never look
            write_script("evil", Span<const byte>(code, 4), payloads[i]);
        } else sfx_payload(payloads[i]);
        items.push_back({assets[i].guid, xxh64(payloads[i].data(), payloads[i].size()), assets[i].type, "x"});
    }
    Array<byte> mp;
    write_manifest(Span<ManifestItem>(items.data(), items.size()), mp);
    ContainerWriter w;
    ContainerWriter::ChunkDesc md;
    md.type = ChunkType::Manifest;
    md.version = chunk_version(MANIFEST_VERSION, 0);
    w.add(md, mp.span());
    Array<byte> ip;
    if (info) {
        write_mod_info(*info, ip);
        ContainerWriter::ChunkDesc d;
        d.type = ChunkType::ModInfo;
        d.version = chunk_version(MOD_INFO_VERSION, 0);
        w.add(d, ip.span());
    }
    for (usize i = 0; i < assets.size(); ++i) {
        ContainerWriter::ChunkDesc d;
        d.type = assets[i].type;
        d.version = chunk_version(assets[i].type == ChunkType::Texture  ? TEXTURE_VERSION
                                  : assets[i].type == ChunkType::Script ? SCRIPT_VERSION
                                                                        : SFX_VERSION,
                                  0);
        d.guid = assets[i].guid;
        w.add(d, payloads[i].span());
    }
    Array<byte> pack;
    w.finish(pack);
    write_bytes(path, pack.span());
    return path;
}

std::string make_mod(const std::string& dir, const char* file, const char* id, const char* game = "",
                     std::vector<PackAsset> assets = {{kTex, ChunkType::Texture, 0x99}}) {
    const ModInfoData info{id, "", "1.0", game, "a test mod"};
    return make_pack(dir + "/" + file, std::move(assets), &info);
}

u8 texture_shade(AssetSystem& as, Guid g) {
    auto h = as.load_sync<Texture>(g);
    REQUIRE(h);
    auto tv = TextureView::read(as.resolve(h.value()));
    REQUIRE(tv);
    const u8 shade = static_cast<u8>(tv.value().pixels(0)[0]);
    as.release(h.value());
    return shade;
}

std::vector<std::string> ids(const ModList& l) {
    std::vector<std::string> out;
    for (const ModEntry& m : l.mods) out.emplace_back(m.id.view());
    return out;
}

}  // namespace

TEST_CASE("mods: ModInfo round-trips, and the reader refuses what it cannot bound") {
    const ModInfoData in{"more-crates", "More Crates", "1.2.0", "ember_step", "Adds crates."};
    Array<byte> p;
    write_mod_info(in, p);
    CHECK(p.size() % CHUNK_ALIGN == 0);
    auto v = ModInfoView::read(p.span());
    REQUIRE(v);
    CHECK(v.value().data.id == "more-crates");
    CHECK(v.value().data.name == "More Crates");
    CHECK(v.value().data.version == "1.2.0");
    CHECK(v.value().data.game == "ember_step");
    CHECK(v.value().data.description == "Adds crates.");

    CHECK_FALSE(ModInfoView::read(p.span().subspan(0, sizeof(ModInfoHeader) - 1)));
    Array<byte> bad = p;
    reinterpret_cast<ModInfoHeader*>(bad.data())->name.size = 0xFFFF;  // past the blob
    CHECK_FALSE(ModInfoView::read(bad.span()));
    bad = p;
    reinterpret_cast<ModInfoHeader*>(bad.data())->strings_size = 0x7FFFFFFF;
    CHECK_FALSE(ModInfoView::read(bad.span()));
    bad = p;
    reinterpret_cast<ModInfoHeader*>(bad.data())->reserved[2] = 1;
    CHECK(ModInfoView::read(bad.span()).error().code == Errc::ReservedNonZero);

    // Untrusted bytes: deterministic mutations never crash, and whatever reads stays in bounds.
    u32 seed = 12345;
    for (int i = 0; i < 1000; ++i) {
        Array<byte> m = p;
        for (int k = 0; k < 3; ++k) {
            seed = seed * 1664525u + 1013904223u;
            m[(seed >> 8) % m.size()] = static_cast<byte>(seed >> 24);
        }
        if (auto r = ModInfoView::read(m.span())) {
            const ModInfoData& d = r.value().data;
            for (StringView s : {d.id, d.name, d.version, d.game, d.description})
                CHECK((s.empty() || (s.data() >= reinterpret_cast<const char*>(m.data()) &&
                                     s.data() + s.size() <= reinterpret_cast<const char*>(m.data()) + m.size())));
        }
    }
}

TEST_CASE("mods: ids are short, lower-case and file-safe") {
    CHECK(is_valid_mod_id("more-crates"));
    CHECK(is_valid_mod_id("a"));
    CHECK(is_valid_mod_id("hd_textures.v2"));
    CHECK_FALSE(is_valid_mod_id(""));
    CHECK_FALSE(is_valid_mod_id(".hidden"));
    CHECK_FALSE(is_valid_mod_id("More"));
    CHECK_FALSE(is_valid_mod_id("a/b"));
    CHECK_FALSE(is_valid_mod_id("a b"));
    CHECK_FALSE(is_valid_mod_id(std::string(65, 'a')));
}

TEST_CASE("mods: without load_order.txt every pack for this game loads, by file name") {
    const std::string dir = tmp_dir("mods_resolve");
    const std::string b = make_mod(dir, "b.mce", "beta");
    const std::string a = make_mod(dir, "a.mce", "alpha", "ember_step");
    const std::string c = make_mod(dir, "c.mce", "gamma", "other_game");
    const std::string d = make_mod(dir, "d.mce", "alpha");  // duplicate id
    const std::string e = make_pack(dir + "/e.mce", {{kTex, ChunkType::Texture, 1}}, nullptr);  // a game pack, not a mod
    const std::string f = dir + "/f.mce";
    write_bytes(f, Span<const byte>(reinterpret_cast<const byte*>("junk"), 4));
    const StringView packs[] = {f, e, d, c, b, a};

    const ModList l = resolve_mods(Span<const StringView>(packs, 6), nullptr, "ember_step");
    CHECK(ids(l) == std::vector<std::string>{"alpha", "beta"});
    CHECK(l.mods[0].game.view() == "ember_step");
    CHECK(l.mods[0].name.view() == "alpha");  // no name: the id
    CHECK(l.mods[1].asset_count == 1);
    REQUIRE(l.skipped.size() == 4);
    CHECK(l.skipped[0].what.view() == c);
    CHECK(l.skipped[0].reason.view().find("another game") != StringView::npos);
    CHECK(l.skipped[1].reason.view().find("duplicate mod id") != StringView::npos);
    CHECK(l.skipped[2].what.view() == e);
    CHECK(l.skipped[3].what.view() == f);

    // No game id on the host: no game check.
    CHECK(ids(resolve_mods(Span<const StringView>(packs, 6), nullptr, "")) == std::vector<std::string>{"alpha", "beta", "gamma"});
}

TEST_CASE("mods: load_order.txt picks the mods and their order") {
    const std::string dir = tmp_dir("mods_order");
    const std::string a = make_mod(dir, "a.mce", "alpha");
    const std::string b = make_mod(dir, "b.mce", "beta");
    const std::string c = make_mod(dir, "c.mce", "gamma");
    const StringView packs[] = {a, b, c};
    const StringView order = "# my mods\r\n  gamma  \n\nalpha # last wins\nmissing\nalpha\n";
    const ModList l = resolve_mods(Span<const StringView>(packs, 3), &order, {});
    CHECK(ids(l) == std::vector<std::string>{"gamma", "alpha"});
    std::vector<std::string> skipped;
    for (const ModSkipped& s : l.skipped) skipped.emplace_back(s.what.view());
    CHECK(skipped == std::vector<std::string>{"missing", "alpha", b});

    const StringView empty;
    CHECK(resolve_mods(Span<const StringView>(packs, 3), &empty, {}).mods.empty());  // an empty file: every mod off
}

TEST_CASE("mods: a mod mounted before the game overrides same-type assets and adds new ones") {
    const std::string dir = tmp_dir("mods_overlay");
    const std::string game = make_pack(dir + "/game.mce", {{kTex, ChunkType::Texture, 0x22}, {kNew, ChunkType::Sfx, 0}}, nullptr);
    const Guid kAdded{0x4102030405060708ull, 1};
    const std::string mod = make_mod(dir, "mod.mce", "retex", "", {{kTex, ChunkType::Texture, 0x99}, {kAdded, ChunkType::Texture, 0x55}});
    const std::string bad = make_mod(dir, "bad.mce", "bad", "", {{kNew, ChunkType::Texture, 0x77}});  // a Sfx GUID as a Texture

    AssetSystem as;
    const StringView roots[] = {bad, mod, game};
    REQUIRE(as.mount(Span<const StringView>(roots, 3)));
    CHECK(texture_shade(as, kTex) == 0x99);    // the mod's texture
    CHECK(texture_shade(as, kAdded) == 0x55);  // new content
    // The type-changing override is ignored: the game's Sfx keeps its GUID.
    CHECK(as.info(as.find(kNew)).type == ChunkType::Sfx);
    auto s = as.load_sync<Sfx>(kNew);
    REQUIRE(s);
    as.release(s.value());

    AssetSystem plain;
    const StringView game_only[] = {game};
    REQUIRE(plain.mount(Span<const StringView>(game_only, 1)));
    CHECK(texture_shade(plain, kTex) == 0x22);
    CHECK(plain.find(kAdded) == kInvalidAssetKey);
}

TEST_CASE("mods: a mod pack cannot carry script bytecode, new or overriding (M-SCRIPT2 0)") {
    const std::string dir = tmp_dir("mods_no_code");
    const Guid kScript{0x5102030405060708ull, 5};
    const Guid kAddedScript{0x6102030405060708ull, 6};
    const std::string game = make_pack(dir + "/game.mce", {{kScript, ChunkType::Script, 0}}, nullptr);
    const std::string mod = make_mod(dir, "mod.mce", "evil", "",
                                     {{kScript, ChunkType::Script, 0}, {kAddedScript, ChunkType::Script, 0}, {kTex, ChunkType::Texture, 0x99}});

    CHECK_FALSE(MountOptions{}.allow_code);  // the default is data-only: code is something a mount asks for
    MountOptions own;
    own.allow_code = true;

    AssetSystem as;
    const StringView mod_root[] = {mod};
    REQUIRE(as.mount(Span<const StringView>(mod_root, 1)));  // default options
    const StringView game_root[] = {game};
    REQUIRE(as.mount(Span<const StringView>(game_root, 1), own));
    CHECK(as.find(kAddedScript) == kInvalidAssetKey);  // the mod's new script never entered the table
    REQUIRE(as.find(kScript) != kInvalidAssetKey);
    CHECK(as.info(as.find(kScript)).path == "x");
    CHECK(as.info(as.find(kScript)).type == ChunkType::Script);
    CHECK(texture_shade(as, kTex) == 0x99);  // its data still loads

    // The same pack mounted without asking for code has no scripts either.
    AssetSystem plain;
    REQUIRE(plain.mount(Span<const StringView>(game_root, 1)));
    CHECK(plain.find(kScript) == kInvalidAssetKey);
    AssetSystem trusted;
    REQUIRE(trusted.mount(Span<const StringView>(game_root, 1), own));
    CHECK(trusted.find(kScript) != kInvalidAssetKey);
}
