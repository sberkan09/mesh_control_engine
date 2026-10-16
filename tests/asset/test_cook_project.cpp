// End-to-end cook of a small project: author -> cook -> mount -> load (the M2 pipeline), plus
// determinism (FR-AST-030), incremental re-cook (FR-AST-080), GUID stability (FR-AST-040),
// dependency errors, reflection-driven scene cook (FR-ENT-060, FR-DX-020) and importers.

#include <doctest/doctest.h>

#include <cstring>
#include <map>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/xxhash.h"
#include "mce/cook/cook.h"
#include "mce/cook/envmap.h"
#include "mce/refl/refl.h"

using namespace mce;
using namespace mce::asset;
using namespace cook_test;
using cook::AssetStatus;
using cook::CookOptions;
using cook::CookReport;
using cook::CookService;

// ------------------------------------------------------------------ reflected test components

namespace cooktest {

struct TestEntity {  // stand-in for mce::Entity until the ECS lands
    u32 index = 0;
    u32 generation = 0;
};

enum class Facing : u8 { Left, Right = 3 };
MCE_ENUM(Facing, Left, Right)

struct Tint {
    math::Color color;
    f32 strength = 1.0f;
};
MCE_REFL_STRUCT(Tint, color, strength)

struct CtTransform {
    math::vec3 position;
    math::quat rotation;
    f32 scale = 1.0f;
};
MCE_COMPONENT(CtTransform, position, rotation, scale)

struct CtSprite {
    AssetRef<Texture> texture;
    AssetRef<Material> material;
    u32 frame = 0;
    Facing facing = Facing::Left;
    FixedString<16> label = "none";
    Tint tint;
    i16 layers[3] = {1, 2, 3};
    u32 cache = 7;
};
MCE_COMPONENT(CtSprite, texture, material, frame, facing, label, tint, layers, (cache, Transient))

struct CtLevel {
    AssetRef<Scene> next;
};
MCE_COMPONENT(CtLevel, next)

struct CtCue {
    AssetRef<Sequence> seq;
};
MCE_COMPONENT(CtCue, seq)

struct CtSound {
    AssetRef<Sfx> clip;
    bool loop = false;
};
MCE_COMPONENT(CtSound, clip, loop)

}  // namespace cooktest

template <>
struct mce::refl::FieldTraits<cooktest::TestEntity> {
    static constexpr bool supported = true;
    static constexpr FieldKind kind = FieldKind::EntityRef;
    static constexpr const char* type_name = "TestEntity";
};

namespace cooktest {
struct CtFollow {
    TestEntity target;
    f32 distance = 2.0f;
};
MCE_COMPONENT(CtFollow, target, distance)
}  // namespace cooktest

// ------------------------------------------------------------------ fixture project

namespace {

constexpr const char* kTexGuid = "a1a1a1a1-0000-4000-8000-000000000001";
constexpr const char* kWavGuid = "a1a1a1a1-0000-4000-8000-000000000002";
constexpr const char* kFontGuid = "a1a1a1a1-0000-4000-8000-000000000003";
constexpr const char* kMatGuid = "a1a1a1a1-0000-4000-8000-000000000004";
constexpr const char* kSceneGuid = "a1a1a1a1-0000-4000-8000-000000000005";

Guid G(const char* s) { return parse_guid(s).value(); }

std::string scene_text(const char* tex_ref = kTexGuid) {
    return std::string(R"({
  "format": "mce-scene",
  "version": 1,
  "guid": ")") + kSceneGuid + R"(",
  "name": "level_01",
  "settings": { "gravity": [0, -9.81, 0] },
  "entities": [
    { "name": "Player",
      "components": {
        "CtTransform": { "position": [1, 2, 3], "scale": 2 },
        "CtSprite": { "texture": "guid:)" + tex_ref + R"(", "material": "guid:)" + kMatGuid + R"(",
                      "frame": 4, "facing": "Right", "label": "hero",
                      "tint": { "color": [1, 0, 0, 1] }, "layers": [7, 8, 9] },
        "CtFollow": { "target": "$1" } } },
    { "name": "Camera",
      "components": {
        "CtTransform": { "position": [0, 0, -10] },
        "CtSound": { "clip": "guid:)" + kWavGuid + R"(", "loop": true },
        "CtFollow": { "target": null, "distance": 5 } } },
    { "name": "Empty" }
  ]
})";
}

std::string material_text() {
    return std::string(R"({ "format": "mce-material", "version": 1, "guid": ")") + kMatGuid +
           R"(", "base_color": [0.5, 0.5, 0.5, 1], "roughness": 0.25, "alpha_mode": "mask", "uv_scale": [8, 0.5],
  "textures": { "base_color": "guid:)" + kTexGuid + R"(" } })";
}

std::string make_project(const char* name) {
    const std::string root = fresh_dir(name);
    const std::string a = root + "/assets";
    write_file(a + "/textures/hero.png", make_gradient_png(5, 3));
    write_file(a + "/textures/hero.png.mceasset", sidecar(kTexGuid, "texture"));
    write_file(a + "/audio/jump.wav", make_wav(22050, 1, 2205, 660.0));
    write_file(a + "/audio/jump.wav.mceasset", sidecar(kWavGuid, "wav", "{ \"loop\": true }"));
    write_file(a + "/fonts/proggy.ttf", read_file(std::string(MCE_TEST_DATA_DIR) + "/fonts/ProggyClean.ttf"));
    write_file(a + "/fonts/proggy.ttf.mceasset", sidecar(kFontGuid, "font", "{ \"size\": 13, \"ranges\": [[32, 126]] }"));
    write_file(a + "/materials/hero.mcmat", material_text());
    write_file(a + "/scenes/level_01.mcscene", scene_text());
    write_file(a + "/README.md", "not an asset");
    return root;
}

const cook::AssetReport* report_for(const CookReport& r, const char* guid) {
    for (const auto& a : r.assets)
        if (a.guid == G(guid)) return &a;
    return nullptr;
}

std::string diag_text(const CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

bool has_diag(const CookReport& r, Errc code, const char* file_suffix, u32 line = 0) {
    for (const auto& d : r.diags.all()) {
        if (d.code != code) continue;
        const std::string f = d.file.c_str();
        if (f.size() >= std::strlen(file_suffix) && f.compare(f.size() - std::strlen(file_suffix), std::string::npos, file_suffix) == 0 &&
            (line == 0 || d.line == line))
            return true;
    }
    return false;
}

std::map<std::string, std::string> snapshot_cooked(const std::string& root) {
    std::map<std::string, std::string> files;
    for (const auto& e : fs::recursive_directory_iterator(root + "/cooked")) {
        if (!e.is_regular_file() || e.path().filename() == ".mcedb") continue;
        files[fs::relative(e.path(), root).generic_string()] = read_file(e.path().generic_string());
    }
    return files;
}

}  // namespace

// ------------------------------------------------------------------ tests

TEST_CASE("cook: full project cooks, then everything is up to date") {
    const std::string root = make_project("cook_full");
    CookService svc;
    const CookReport r1 = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r1));
    REQUIRE(r1.ok());
    CHECK(r1.cooked == 5);
    CHECK(r1.assets.size() == 5);
    // Topological order: the texture precedes the material, which precedes the scene.
    usize tex_at = 0, mat_at = 0, scene_at = 0;
    for (usize i = 0; i < r1.assets.size(); ++i) {
        if (r1.assets[i].guid == G(kTexGuid)) tex_at = i;
        if (r1.assets[i].guid == G(kMatGuid)) mat_at = i;
        if (r1.assets[i].guid == G(kSceneGuid)) scene_at = i;
    }
    CHECK(tex_at < mat_at);
    CHECK(mat_at < scene_at);
    CHECK(fs::exists(root + "/cooked/manifest.mce"));
    CHECK(fs::exists(root + "/cooked/.mcedb"));

    const CookReport r2 = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(r2.ok());
    CHECK(r2.cooked == 0);
    CHECK(r2.up_to_date == 5);
    CHECK(r2.manifest_hash == r1.manifest_hash);
}

TEST_CASE("cook: determinism — deleting cooked/ and re-cooking is byte-identical (FR-AST-030)") {
    const std::string root = make_project("cook_determinism");
    CookService svc;
    CookOptions opt;
    opt.pack = true;
    const CookReport r1 = svc.cook(root.c_str(), opt);
    INFO(diag_text(r1));
    REQUIRE(r1.ok());
    const auto files1 = snapshot_cooked(root);
    const std::string pack1 = read_file(root + "/assets.mce");
    REQUIRE(!pack1.empty());

    fs::remove_all(root + "/cooked");
    fs::remove(root + "/assets.mce");
    const CookReport r2 = svc.cook(root.c_str(), opt);
    REQUIRE(r2.ok());
    CHECK(r2.cooked == 5);
    CHECK(r2.manifest_hash == r1.manifest_hash);
    CHECK(r2.pack_hash == r1.pack_hash);
    CHECK(snapshot_cooked(root) == files1);
    CHECK(read_file(root + "/assets.mce") == pack1);

    opt.force = true;
    const CookReport r3 = svc.cook(root.c_str(), opt);
    REQUIRE(r3.ok());
    CHECK(r3.cooked == 5);
    CHECK(r3.pack_hash == r1.pack_hash);

    // A second project with the same sources at a different path cooks to the same bytes: no
    // path or environment residue in any output.
    const std::string other = make_project("cook_determinism_elsewhere");
    const CookReport r4 = svc.cook(other.c_str(), opt);
    REQUIRE(r4.ok());
    CHECK(r4.pack_hash == r1.pack_hash);
}

TEST_CASE("cook: incremental — a changed texture re-cooks itself and its dependents only") {
    const std::string root = make_project("cook_incremental");
    CookService svc;
    REQUIRE(svc.cook(root.c_str(), CookOptions{}).ok());
    write_file(root + "/assets/textures/hero.png", make_gradient_png(5, 3, 9));
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());
    CHECK(report_for(r, kTexGuid)->status == AssetStatus::Cooked);
    CHECK(report_for(r, kMatGuid)->status == AssetStatus::Cooked);    // dependency output hash changed
    CHECK(report_for(r, kSceneGuid)->status == AssetStatus::Cooked);  // references the texture directly
    CHECK(report_for(r, kWavGuid)->status == AssetStatus::UpToDate);
    CHECK(report_for(r, kFontGuid)->status == AssetStatus::UpToDate);

    // A material edit changes its output hash: the scene that references it re-cooks.
    write_file(root + "/assets/materials/hero.mcmat", material_text() + "\n// edited\n");
    const CookReport r2 = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(r2.ok());
    CHECK(report_for(r2, kMatGuid)->status == AssetStatus::Cooked);  // source hash changed
    CHECK(report_for(r2, kSceneGuid)->status == AssetStatus::UpToDate);  // same material bytes out
    std::string mat = material_text();
    mat.replace(mat.find("0.25"), 4, "0.75");
    write_file(root + "/assets/materials/hero.mcmat", mat);
    const CookReport r3 = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(r3.ok());
    CHECK(report_for(r3, kMatGuid)->status == AssetStatus::Cooked);
    CHECK(report_for(r3, kSceneGuid)->status == AssetStatus::Cooked);  // dependency output hash changed
    CHECK(report_for(r3, kTexGuid)->status == AssetStatus::UpToDate);

    // Sidecar settings are part of the key.
    write_file(root + "/assets/textures/hero.png.mceasset", sidecar(kTexGuid, "texture", "{ \"mips\": false }"));
    const CookReport r4 = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(r4.ok());
    CHECK(report_for(r4, kTexGuid)->status == AssetStatus::Cooked);
}

TEST_CASE("cook: a corrupt cook DB is dropped and the cook still succeeds") {
    const std::string root = make_project("cook_db_corrupt");
    CookService svc;
    const CookReport r1 = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(r1.ok());
    std::string db = read_file(root + "/cooked/.mcedb");
    db[db.size() / 2] ^= 0x55;
    write_file(root + "/cooked/.mcedb", db);
    const CookReport r2 = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(r2.ok());
    CHECK(r2.manifest_hash == r1.manifest_hash);
    const CookReport r3 = svc.cook(root.c_str(), CookOptions{});
    CHECK(r3.up_to_date == 5);  // the rewritten DB is whole again
}

TEST_CASE("cook: author -> cook -> mount -> load the scene (M2 pipeline)") {
    const std::string root = make_project("cook_load");
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());

    AssetSystem as;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {cooked};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    CHECK(as.asset_count() == 5);

    auto sh = as.load_sync<Scene>(G(kSceneGuid));
    REQUIRE(sh);
    auto sv_r = SceneView::read(as.resolve(sh.value()));
    REQUIRE(sv_r);
    const SceneView& sv = sv_r.value();
    CHECK(sv.header->entity_count == 3);
    CHECK(sv.entity_name(0) == "Player");
    CHECK(sv.entity_name(2) == "Empty");
    REQUIRE(sv.header->column_count == 4);
    // Columns sorted by type name.
    CHECK(sv.columns[0].type_id == StringId("CtFollow").value);
    CHECK(sv.columns[1].type_id == StringId("CtSound").value);
    CHECK(sv.columns[2].type_id == StringId("CtSprite").value);
    CHECK(sv.columns[3].type_id == StringId("CtTransform").value);

    const SceneColumn* tc = sv.find_column(StringId("CtTransform").value);
    REQUIRE(tc);
    CHECK(tc->row_count == 2);
    CHECK(tc->type_size == sizeof(cooktest::CtTransform));
    const u32 tci = static_cast<u32>(tc - sv.columns.data());
    CHECK(sv.rows(tci)[0] == 0);
    CHECK(sv.rows(tci)[1] == 1);
    cooktest::CtTransform t[2];
    std::memcpy(t, sv.data(tci).data(), sizeof(t));
    CHECK(t[0].position.y == 2.0f);
    CHECK(t[0].scale == 2.0f);
    CHECK(t[0].rotation.w == 1.0f);  // default member value kept
    CHECK(t[1].position.z == -10.0f);
    CHECK(t[1].scale == 1.0f);

    const SceneColumn* spc = sv.find_column(StringId("CtSprite").value);
    REQUIRE(spc);
    const u32 spi = static_cast<u32>(spc - sv.columns.data());
    cooktest::CtSprite sp;
    std::memcpy(static_cast<void*>(&sp), sv.data(spi).data(), sizeof(sp));
    CHECK(sp.texture.guid == G(kTexGuid));
    CHECK(!sp.texture.handle.valid());  // stamped by the loader's fixup pass
    CHECK(sp.frame == 4);
    CHECK(sp.facing == cooktest::Facing::Right);
    CHECK(sp.label == "hero");
    CHECK(sp.tint.color.r == 1.0f);
    CHECK(sp.tint.color.g == 0.0f);
    CHECK(sp.tint.strength == 1.0f);
    CHECK(sp.layers[2] == 9);
    CHECK(sp.cache == 7);  // transient: default

    // Asset refs: unique, sorted, typed; fixups point at the AssetRef fields.
    REQUIRE(sv.header->asset_ref_count == 3);
    for (u32 i = 1; i < sv.asset_refs.size(); ++i) CHECK(sv.asset_refs[i - 1].guid() < sv.asset_refs[i].guid());
    CHECK(sv.asset_refs[0].guid() == G(kTexGuid));
    CHECK(sv.asset_refs[0].type_id == StringId("Texture").value);
    REQUIRE(sv.header->asset_fixup_count == 3);
    for (const SceneFixup& f : sv.asset_fixups) {
        Guid g;
        std::memcpy(&g, sv.data(f.column).data() + f.row * sv.columns[f.column].type_size + f.byte_offset, sizeof(g));
        CHECK(g == sv.asset_refs[f.index].guid());
    }
    // Entity refs: "$1" -> 1, null -> 0xFFFFFFFF.
    REQUIRE(sv.header->entity_fixup_count == 2);
    const u32 fci = static_cast<u32>(sv.find_column(StringId("CtFollow").value) - sv.columns.data());
    cooktest::CtFollow follow[2];
    std::memcpy(static_cast<void*>(follow), sv.data(fci).data(), sizeof(follow));
    CHECK(follow[0].target.index == 1);
    CHECK(follow[1].target.index == 0xFFFFFFFFu);
    CHECK(follow[1].distance == 5.0f);

    // Every referenced asset loads through the same table (scene load pass 1).
    for (const SceneAssetRef& ref : sv.asset_refs) {
        const AssetKey k = as.find(ref.guid());
        REQUIRE(k != kInvalidAssetKey);
        ChunkType expected;
        const char* names[] = {"Texture", "Material", "Sfx"};
        for (const char* n : names)
            if (StringId(n).value == ref.type_id) REQUIRE(chunk_type_for_asset_name(n, expected));
        CHECK(as.info(k).type == expected);
    }
    auto th = as.load_sync<Texture>(G(kTexGuid));
    REQUIRE(th);
    auto tv = TextureView::read(as.resolve(th.value()));
    REQUIRE(tv);
    CHECK(tv.value().header->width == 5);
    CHECK(tv.value().header->mip_count == 3);  // 5x3, 2x1, 1x1
    CHECK((tv.value().header->flags & TextureFlags::Srgb) != 0);
    CHECK(static_cast<u8>(tv.value().pixels(0)[4]) == 50);  // texel (1, 0) red = 1 * 50

    auto snd = as.load_sync<Sfx>(G(kWavGuid));
    REQUIRE(snd);
    auto sfx = SfxView::read(as.resolve(snd.value()));
    REQUIRE(sfx);
    CHECK(sfx.value().header->sample_rate == 48000);
    CHECK(sfx.value().header->frame_count == 4800);  // 0.1 s at 22.05 kHz -> 48 kHz
    CHECK((sfx.value().header->flags & SfxFlags::Looping) != 0);
    CHECK(sfx.value().header->loop_end == 4800);
    f32 peak = 0;
    for (f32 s : sfx.value().samples) peak = std::max(peak, std::fabs(s));
    CHECK(peak == doctest::Approx(0.891250938f));

    auto fh = as.load_sync<Font>(G(kFontGuid));
    REQUIRE(fh);
    auto fv = FontView::read(as.resolve(fh.value()));
    REQUIRE(fv);
    CHECK(fv.value().glyphs.size() == 95);
    const FontGlyph* a = fv.value().find('A');
    REQUIRE(a);
    CHECK(a->x1 > a->x0);
    CHECK(a->xadvance > 0);
    u32 coverage = 0;
    for (byte b : fv.value().atlas) coverage += static_cast<u8>(b) != 0;
    CHECK(coverage > 100);

    auto mh = as.load_sync<Material>(G(kMatGuid));
    REQUIRE(mh);
    auto mat = MaterialChunk::read(as.resolve(mh.value()));
    REQUIRE(mat);
    CHECK(mat.value()->roughness == 0.25f);
    CHECK(mat.value()->texture(MaterialSlot::BaseColor) == G(kTexGuid));
    CHECK((mat.value()->flags & MaterialFlags::AlphaMask) != 0);
    f32 uv[2];
    mat.value()->uv_scale(uv);
    CHECK(uv[0] == 8.0f);  // halves hold these exactly
    CHECK(uv[1] == 0.5f);

    for (AnyHandle h : {AnyHandle(sh.value()), AnyHandle(th.value()), AnyHandle(snd.value()), AnyHandle(fh.value()), AnyHandle(mh.value())})
        as.release(h);
}

TEST_CASE("cook: --textures astc stores ASTC 4x4 blocks, re-cooks on a switch, and is deterministic") {
    const std::string root = make_project("cook_astc");
    CookService svc;
    REQUIRE(svc.cook(root.c_str(), CookOptions{}).ok());
    CookOptions astc;
    astc.textures = cook::TextureCompression::Astc4x4;
    astc.pack = true;
    const CookReport r1 = svc.cook(root.c_str(), astc);
    INFO(diag_text(r1));
    REQUIRE(r1.ok());
    CHECK(r1.cooked == 5);  // a different texture storage is a different tool version: everything re-cooks

    AssetSystem as;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {cooked};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    auto th = as.load_sync<Texture>(G(kTexGuid));
    REQUIRE(th);
    auto tv = TextureView::read(as.resolve(th.value()));
    REQUIRE(tv);
    CHECK(tv.value().header->format == TextureFormat::ASTC_4x4);
    REQUIRE(tv.value().mips.size() == 3);  // 5x3, 2x1, 1x1
    CHECK(tv.value().mips[0].layer_size == 2 * 1 * 16);  // ceil(5/4) x ceil(3/4) blocks of 16 bytes
    CHECK(tv.value().mips[2].layer_size == 16);
    as.release(AnyHandle(th.value()));

    const std::string pack1 = read_file(root + "/assets.mce");
    fs::remove_all(root + "/cooked");
    fs::remove(root + "/assets.mce");
    const CookReport r2 = svc.cook(root.c_str(), astc);
    REQUIRE(r2.ok());
    CHECK(r2.pack_hash == r1.pack_hash);
    CHECK(read_file(root + "/assets.mce") == pack1);
}

TEST_CASE("cook: a baked environment (.mcenv) cooks to a linear RGBA8 array, never ASTC") {
    const std::string root = make_project("cook_envmap");
    constexpr const char* kEnvGuid = "a1a1a1a1-0000-4000-8000-0000000000e1";
    Array<u8> px;
    for (u32 i = 0; i < 4 * 2 * 3; ++i)
        for (u8 c : {u8(i), u8(2 * i), u8(255 - i), u8(17)}) px.push_back(c);
    Array<byte> file;
    cook::write_envmap(4, 2, 3, px.span(), file);
    write_file(root + "/assets/probes/hall.mcenv", std::string(reinterpret_cast<const char*>(file.data()), file.size()));
    write_file(root + "/assets/probes/hall.mcenv.mceasset", sidecar(kEnvGuid, "envmap"));
    CookService svc;
    CookOptions astc;
    astc.textures = cook::TextureCompression::Astc4x4;
    const CookReport r = svc.cook(root.c_str(), astc);
    INFO(diag_text(r));
    REQUIRE(r.ok());

    AssetSystem as;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {cooked};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    auto th = as.load_sync<Texture>(G(kEnvGuid));
    REQUIRE(th);
    auto tv = TextureView::read(as.resolve(th.value()));
    REQUIRE(tv);
    const TextureHeader& h = *tv.value().header;
    CHECK(h.format == TextureFormat::RGBA8);
    CHECK(h.width == 4);
    CHECK(h.height == 2);
    CHECK(h.layers == 3);
    CHECK(h.mip_count == 1);
    CHECK(h.flags == 0);  // linear: RGBM is not color data
    const Span<const byte> layer2 = tv.value().pixels(0, 2);
    REQUIRE(layer2.size() == 4 * 2 * 4);
    CHECK(std::memcmp(layer2.data(), px.data() + 2 * 4 * 2 * 4, layer2.size()) == 0);
    as.release(AnyHandle(th.value()));

    file.resize(file.size() - 1);  // truncated
    write_file(root + "/assets/probes/hall.mcenv", std::string(reinterpret_cast<const char*>(file.data()), file.size()));
    const CookReport bad = svc.cook(root.c_str(), CookOptions{});
    CHECK(!bad.ok());
    CHECK(diag_text(bad).find("does not match its header") != std::string::npos);
}

TEST_CASE("cook: scene source hash is over the canonical form") {
    const std::string root = make_project("cook_canonical");
    CookService svc;
    const CookReport r1 = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(r1.ok());
    // Reformat: whitespace and key order change, content does not.
    std::string s = scene_text();
    s.insert(s.find("\"format\""), "\n\n   ");
    write_file(root + "/assets/scenes/level_01.mcscene", s);
    const CookReport r2 = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(r2.ok());
    CHECK(report_for(r2, kSceneGuid)->status == AssetStatus::Cooked);
    CHECK(report_for(r2, kSceneGuid)->output_hash == report_for(r1, kSceneGuid)->output_hash);
}

TEST_CASE("cook: missing sidecar is an error; --adopt writes one") {
    const std::string root = make_project("cook_adopt");
    write_file(root + "/assets/textures/extra.png", make_gradient_png(2, 2));
    CookService svc;
    const CookReport r1 = svc.cook(root.c_str(), CookOptions{});
    CHECK(!r1.ok());
    CHECK(has_diag(r1, Errc::MissingSidecar, "textures/extra.png"));
    CHECK(r1.cooked == 5);  // everything else still cooks

    CookOptions opt;
    opt.adopt = true;
    const CookReport r2 = svc.cook(root.c_str(), opt);
    INFO(diag_text(r2));
    CHECK(r2.ok());
    CHECK(fs::exists(root + "/assets/textures/extra.png.mceasset"));
    CHECK(svc.cook(root.c_str(), CookOptions{}).ok());
}

TEST_CASE("cook: the missing-sidecar error names the fix, over MCP and on the command line") {
    const std::string root = make_project("cook_adopt_hint");
    write_file(root + "/assets/scripts/new_thing.luau", "return 1\n");
    write_file(root + "/assets/textures/extra.png", make_gradient_png(2, 2));
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    CHECK(!r.ok());
    const std::string text = diag_text(r);
    CHECK(has_diag(r, Errc::MissingSidecar, "scripts/new_thing.luau"));
    CHECK(text.find("cook_run {\"adopt\": true}") != std::string::npos);
    CHECK(text.find("mce cook <project> --adopt") != std::string::npos);
    // The report's failed-file list carries it too, once per file.
    u32 named = 0;
    for (const auto& f : r.failed_files)
        if (std::string(f.file.c_str()).find("new_thing.luau") != std::string::npos && f.code == Errc::MissingSidecar) ++named;
    CHECK(named == 1);
}

TEST_CASE("cook: skip_errors cooks the good files and lists the bad ones; no pack from a broken run") {
    const std::string root = make_project("cook_skip_errors");
    write_file(root + "/assets/textures/extra.png", make_gradient_png(2, 2));                    // no sidecar
    write_file(root + "/assets/textures/broken.png", "this is not a png");                      // importer fails
    write_file(root + "/assets/textures/broken.png.mceasset", sidecar("a1a1a1a1-0000-4000-8000-0000000000b1", "texture"));
    CookOptions opt;
    opt.skip_errors = true;
    opt.pack = true;
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), opt);
    INFO(diag_text(r));
    CHECK(!r.ok());
    CHECK(r.cooked == 5);  // the five good assets of the fixture
    CHECK(r.failed == 1);
    bool extra = false, broken = false;
    for (const auto& f : r.failed_files) {
        const std::string name = f.file.c_str();
        extra = extra || (name == "assets/textures/extra.png" && f.code == Errc::MissingSidecar);
        broken = broken || name == "assets/textures/broken.png";
    }
    CHECK(extra);
    CHECK(broken);
    CHECK(!r.pack_written);
    CHECK(!fs::exists(root + "/assets.mce"));
    CHECK(fs::exists(root + "/cooked/manifest.mce"));
    CHECK(report_for(r, kSceneGuid)->status == AssetStatus::Cooked);
}

TEST_CASE("cook: paths cooks only the named subtree and what it depends on, and never the pack") {
    const std::string root = make_project("cook_paths");
    write_file(root + "/assets/textures/extra.png", make_gradient_png(2, 2));  // unregistered: not part of this run
    CookOptions opt;
    opt.paths.push_back(cook::String("materials"));  // a folder: the material and the texture it needs
    opt.pack = true;
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), opt);
    INFO(diag_text(r));
    CHECK(r.ok());  // extra.png is outside the paths, so its missing sidecar is not this run's error
    CHECK(r.subset);
    CHECK(r.cooked == 2);
    CHECK(r.skipped == 3);
    CHECK(report_for(r, kMatGuid)->status == AssetStatus::Cooked);
    CHECK(report_for(r, kTexGuid)->status == AssetStatus::Cooked);  // a dependency of the material
    CHECK(report_for(r, kWavGuid)->status == AssetStatus::Skipped);
    CHECK(report_for(r, kSceneGuid)->status == AssetStatus::Skipped);
    CHECK(!r.pack_written);
    CHECK(!fs::exists(root + "/assets.mce"));
    CHECK(diag_text(r).find("pack not rewritten") != std::string::npos);

    // The rest is untouched: a later full cook cooks exactly what the subset left.
    const CookReport full = svc.cook(root.c_str(), CookOptions{});
    CHECK(report_for(full, kMatGuid)->status == AssetStatus::UpToDate);
    CHECK(report_for(full, kTexGuid)->status == AssetStatus::UpToDate);
    CHECK(report_for(full, kWavGuid)->status == AssetStatus::Cooked);
    CHECK(report_for(full, kSceneGuid)->status == AssetStatus::Cooked);

    // A subset after a full cook keeps the manifest whole: same manifest hash, skipped assets still listed.
    CookOptions one;
    one.paths.push_back(cook::String("assets/audio/jump.wav.mceasset"));  // the prefix and the sidecar spelling are accepted
    const CookReport r2 = svc.cook(root.c_str(), one);
    CHECK(r2.up_to_date == 1);
    CHECK(r2.skipped == 4);
    CHECK(r2.manifest_hash == full.manifest_hash);

    CookOptions none;
    none.paths.push_back(cook::String("nope/missing.png"));
    const CookReport r3 = svc.cook(root.c_str(), none);
    CHECK(!r3.ok());
    CHECK(has_diag(r3, Errc::NotFound, "nope/missing.png"));
}

TEST_CASE("cook: an unknown component or field says the module is stale and how to refresh it") {
    const std::string root = make_project("cook_stale_module");
    std::string s = scene_text();
    s.insert(s.find("\"CtTransform\": { \"position\": [1, 2, 3]"), "\"BrandNewThing\": { \"hp\": 3 },\n        ");
    write_file(root + "/assets/scenes/level_01.mcscene", s);
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    const std::string text = diag_text(r);
    CHECK(!r.ok());
    CHECK(text.find("unknown component type 'BrandNewThing'") != std::string::npos);
    CHECK(text.find("module_build") != std::string::npos);
    CHECK(text.find("restart the MCP server") != std::string::npos);

    std::string t = scene_text();
    t.insert(t.find("\"frame\": 4"), "\"stamina\": 9, ");
    write_file(root + "/assets/scenes/level_01.mcscene", t);
    const std::string field = diag_text(svc.cook(root.c_str(), CookOptions{}));
    CHECK(field.find("unknown field 'stamina' on CtSprite") != std::string::npos);
    CHECK(field.find("restart the MCP server") != std::string::npos);
}

TEST_CASE("cook: duplicate GUIDs are reported with both files") {
    const std::string root = make_project("cook_dup");
    write_file(root + "/assets/textures/copy.png", make_gradient_png(2, 2));
    write_file(root + "/assets/textures/copy.png.mceasset", sidecar(kTexGuid, "texture"));
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    CHECK(!r.ok());
    CHECK(has_diag(r, Errc::DuplicateGuid, "textures/hero.png"));
}

TEST_CASE("cook: external rename is auto-repaired by content hash; references keep resolving") {
    const std::string root = make_project("cook_rename");
    CookService svc;
    REQUIRE(svc.cook(root.c_str(), CookOptions{}).ok());
    fs::create_directories(root + "/assets/art");
    fs::rename(root + "/assets/textures/hero.png", root + "/assets/art/hero_moved.png");  // sidecar left behind
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());
    CHECK(fs::exists(root + "/assets/art/hero_moved.png.mceasset"));
    CHECK(!fs::exists(root + "/assets/textures/hero.png.mceasset"));
    CHECK(report_for(r, kTexGuid)->path.view() == "art/hero_moved.png");

    // Registry move (editor rename) keeps the GUID too.
    cook::AssetRegistry reg;
    cook::DiagList diags;
    cook::ImporterRegistry importers;
    cook::ImporterRegistry::add_builtins(importers);
    REQUIRE(reg.scan(root.c_str(), importers, false, diags));
    REQUIRE(reg.move(G(kTexGuid), "art/renamed.png"));
    CHECK(fs::exists(root + "/assets/art/renamed.png"));
    CHECK(fs::exists(root + "/assets/art/renamed.png.mceasset"));
    CHECK(reg.guid_for_path("art/renamed.png").value() == G(kTexGuid));
    CHECK(svc.cook(root.c_str(), CookOptions{}).ok());
}

TEST_CASE("cook: missing and mistyped references name file:line:col") {
    const std::string root = make_project("cook_refs");
    std::string s = scene_text("a1a1a1a1-0000-4000-8000-0000000000ff");
    write_file(root + "/assets/scenes/level_01.mcscene", s);
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    CHECK(!r.ok());
    CHECK(has_diag(r, Errc::MissingRef, "scenes/level_01.mcscene", 11));
    CHECK(report_for(r, kSceneGuid)->status == AssetStatus::Blocked);

    write_file(root + "/assets/scenes/level_01.mcscene", scene_text(kWavGuid));  // a sound in a texture slot
    const CookReport r2 = svc.cook(root.c_str(), CookOptions{});
    CHECK(!r2.ok());
    CHECK(has_diag(r2, Errc::MissingRef, "scenes/level_01.mcscene", 11));
    CHECK(diag_text(r2).find("expected Texture") != std::string::npos);
}

TEST_CASE("cook: scenes may name each other in a loop (scene references are links, not dependencies)") {
    const std::string root = make_project("cook_scene_links");
    const char* a = "b2b2b2b2-0000-4000-8000-00000000000a";
    const char* b = "b2b2b2b2-0000-4000-8000-00000000000b";
    auto level = [](const char* self, const char* next) {
        return std::string("{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"") + self +
               "\", \"entities\": [ { \"components\": { \"CtLevel\": { \"next\": \"guid:" + next + "\" } } } ] }";
    };
    write_file(root + "/assets/scenes/a.mcscene", level(a, b));
    write_file(root + "/assets/scenes/b.mcscene", level(b, a));
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    CHECK(r.ok());
    CHECK(r.cooked == 7);
    // Still checked: a link to a scene that is not there, or to something that is not a scene.
    write_file(root + "/assets/scenes/b.mcscene", level(b, "b2b2b2b2-0000-4000-8000-0000000000ff"));
    const CookReport r2 = svc.cook(root.c_str(), CookOptions{});
    CHECK(!r2.ok());
    CHECK(diag_text(r2).find("missing reference") != std::string::npos);
    write_file(root + "/assets/scenes/b.mcscene", level(b, kTexGuid));
    CHECK(diag_text(svc.cook(root.c_str(), CookOptions{})).find("expected Scene") != std::string::npos);
}

TEST_CASE("cook: dependency cycles are reported once, naming the cycle") {
    // A prefab whose sequence spawns the prefab: each cook needs the other's output first.
    const std::string root = make_project("cook_cycle");
    const char* prefab = "b2b2b2b2-0000-4000-8000-00000000000c";
    const char* seq = "b2b2b2b2-0000-4000-8000-00000000000d";
    write_file(root + "/assets/spark.mcprefab", std::string("{ \"format\": \"mce-prefab\", \"version\": 1, \"guid\": \"") + prefab +
                                                    "\", \"entities\": [ { \"components\": { \"CtCue\": { \"seq\": \"guid:" + seq +
                                                    "\" } } } ] }");
    write_file(root + "/assets/burst.mcseq", std::string("{ \"format\": \"mce-sequence\", \"version\": 1, \"guid\": \"") + seq +
                                                 "\", \"steps\": [ { \"spawn\": \"guid:" + prefab + "\" } ] }");
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    CHECK(!r.ok());
    u32 cycles = 0;
    for (const auto& d : r.diags.all()) cycles += d.code == Errc::DependencyCycle;
    CHECK(cycles == 1);
    CHECK(diag_text(r).find("spark.mcprefab") != std::string::npos);
    CHECK(diag_text(r).find("burst.mcseq") != std::string::npos);
    CHECK(r.cooked == 5);  // the rest of the project is unaffected
}

TEST_CASE("cook: scene schema errors are batched with positions") {
    const std::string root = make_project("cook_scene_errors");
    const std::string s = std::string(R"({ "format": "mce-scene", "version": 1, "guid": ")") + kSceneGuid + R"(",
  "entities": [
    { "components": { "NoSuchComponent": {} } },
    { "components": { "CtTransform": { "positon": [0, 0, 0] } } },
    { "components": { "CtSprite": { "cache": 3 } } },
    { "components": { "CtSprite": { "frame": -1, "facing": "Up", "label": "much too long for sixteen" } } },
    { "components": { "CtFollow": { "target": "$99" } } },
    { "components": { "CtTransform": { "position": [0, 0] } } }
  ] })";
    write_file(root + "/assets/scenes/level_01.mcscene", s);
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    CHECK(!r.ok());
    for (u32 line : {3u, 4u, 5u, 6u, 7u, 8u}) {
        const bool found = has_diag(r, Errc::ParseError, "level_01.mcscene", line) || has_diag(r, Errc::NotFound, "level_01.mcscene", line);
        CHECK_MESSAGE(found, "line ", line);
    }
    CHECK(r.diags.error_count() >= 8);
    CHECK(report_for(r, kSceneGuid)->status == AssetStatus::Failed);

    // An older scene version asks for an editor re-save.
    write_file(root + "/assets/scenes/level_01.mcscene",
               std::string(R"({ "format": "mce-scene", "version": 0, "guid": ")") + kSceneGuid + "\" }");
    const CookReport r2 = svc.cook(root.c_str(), CookOptions{});
    CHECK(has_diag(r2, Errc::VersionTooNew, "level_01.mcscene"));
    CHECK(diag_text(r2).find("re-save") != std::string::npos);
}

TEST_CASE("cook: a failed asset keeps serving its last good output") {
    const std::string root = make_project("cook_last_good");
    CookService svc;
    const CookReport r1 = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(r1.ok());
    write_file(root + "/assets/materials/hero.mcmat", "{ \"format\": \"mce-material\", \"version\": 1, \"guid\": \"" + std::string(kMatGuid) +
                                                          "\", \"roughness\": \"rough\" }");
    const CookReport r2 = svc.cook(root.c_str(), CookOptions{});
    CHECK(!r2.ok());
    CHECK(report_for(r2, kMatGuid)->status == AssetStatus::Failed);
    CHECK(report_for(r2, kMatGuid)->output_hash == report_for(r1, kMatGuid)->output_hash);
    CHECK(r2.manifest_hash == r1.manifest_hash);
}

TEST_CASE("cook: importer setting validation") {
    const std::string root = make_project("cook_settings");
    write_file(root + "/assets/audio/jump.wav.mceasset", sidecar(kWavGuid, "wav", "{ \"loop\": true, \"loop_start\": 5000, \"loop_end\": 10 }"));
    write_file(root + "/assets/textures/hero.png.mceasset", sidecar(kTexGuid, "texture", "{ \"srgbb\": true }"));
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    CHECK(has_diag(r, Errc::ImportFailed, "audio/jump.wav"));
    CHECK(has_diag(r, Errc::ParseError, "textures/hero.png"));
    CHECK(report_for(r, kMatGuid)->status == AssetStatus::Blocked);  // depends on the failed texture
}

TEST_CASE("material uv_scale packs into two halves; unset means [1, 1]") {
    MaterialChunk m{};
    f32 uv[2];
    m.uv_scale(uv);
    CHECK(uv[0] == 1.0f);
    CHECK(uv[1] == 1.0f);
    CHECK(MaterialChunk::pack_uv_scale(1.0f, 1.0f) == 0u);
    m.uv_scale_half = MaterialChunk::pack_uv_scale(3.3f, 120.0f);
    m.uv_scale(uv);
    CHECK(uv[0] == doctest::Approx(3.3f).epsilon(0.001));
    CHECK(uv[1] == doctest::Approx(120.0f).epsilon(0.001));
}

TEST_CASE("material: a height map, height_scale and parallax_steps cook into MaterialExt (M-VISUAL3 2); bad keys fail") {
    const std::string root = make_project("cook_material_height");
    auto write = [&](const std::string& extra) {
        write_file(root + "/assets/materials/hero.mcmat",
                   std::string(R"({ "format": "mce-material", "version": 1, "guid": ")") + kMatGuid + R"(", )" + extra + " }");
    };
    write(std::string(R"("height_scale": 0.04, "parallax_steps": [6, 40], "textures": { "base_color": "guid:)") + kTexGuid +
          R"(", "height": "guid:)" + kTexGuid + R"(" })");
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(r.ok());
    AssetSystem as;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)).has_value());
    auto mh = as.load_sync<Material>(G(kMatGuid));
    REQUIRE(mh);
    const Span<const byte> payload = as.resolve(mh.value());
    const MaterialExt e = MaterialExt::of_material(payload);
    CHECK(e.parallax());
    CHECK(e.height() == G(kTexGuid));
    CHECK(e.height_scale == doctest::Approx(0.04f));
    CHECK(e.parallax_steps == (6u | 40u << 16));
    CHECK_FALSE(e.any());  // parallax is not a lobe
    CHECK(MaterialChunk::read(payload).value()->texture(MaterialSlot::BaseColor) == G(kTexGuid));  // 'height' is not a chunk slot
    as.release(AnyHandle(mh.value()));

    const char* bad[] = {
        R"("height_scale": 0.04)",                                                        // no height map to scale
        R"("height_scale": 0.9, "textures": { "height": "guid:a1a1a1a1-0000-4000-8000-000000000001" })",  // out of range
        R"("parallax_steps": [2, 40])", R"("parallax_steps": [40, 8])", R"("parallax_steps": 8)", R"("parallax_steps": [8, 65])",
        R"("parallax_steps": [8.5, 32])", R"("height_scale": 0.04, "textures": { "height": null })",
        R"("textures": { "height": "guid:a1a1a1a1-0000-4000-8000-000000000004" })",     // a material, not a texture
    };
    for (const char* extra : bad) {
        write(extra);
        const CookReport b = svc.cook(root.c_str(), CookOptions{});
        CHECK_MESSAGE(!b.ok(), extra);
        CHECK(report_for(b, kMatGuid)->status != AssetStatus::Cooked);
    }

    // A height map without a scale cooks and is off.
    write(std::string(R"("textures": { "height": "guid:)") + kTexGuid + R"(" })");
    REQUIRE(svc.cook(root.c_str(), CookOptions{}).ok());
}

TEST_CASE("material: a height map imported as sRGB warns, naming the texture; srgb false is silent (M-VISUAL3 2)") {
    const std::string root = make_project("cook_material_height_srgb");
    const char* linear_guid = "a1a1a1a1-0000-4000-8000-0000000000a5";
    write_file(root + "/assets/textures/disp.png", make_gradient_png(5, 3));
    write_file(root + "/assets/textures/disp.png.mceasset", sidecar(linear_guid, "texture", R"({ "srgb": false })"));
    auto material = [&](const char* tex) {
        write_file(root + "/assets/materials/hero.mcmat",
                   std::string(R"({ "format": "mce-material", "version": 1, "guid": ")") + kMatGuid + R"(", "height_scale": 0.04, "textures": { "height": "guid:)" +
                       tex + R"(" } })");
    };
    auto height_warnings = [](const CookReport& r) {
        u32 n = 0;
        for (const auto& d : r.diags.all())
            n += d.severity == cook::Severity::Warning && std::string(d.message.c_str()).find("height map assets/textures/hero.png") != std::string::npos;
        return n;
    };
    CookService svc;
    material(kTexGuid);  // hero.png: no "srgb" setting, which defaults to true
    const CookReport warned = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(warned.ok());
    CHECK(height_warnings(warned) == 1);
    material(linear_guid);
    const CookReport quiet = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(quiet.ok());
    CHECK(height_warnings(quiet) == 0);
}

TEST_CASE("material: the M-VISUAL2 4 lobes cook after the base block; a plain material has none; bad ranges fail") {
    const std::string root = make_project("cook_material_ext");
    write_file(root + "/assets/materials/hero.mcmat",
               std::string(R"({ "format": "mce-material", "version": 1, "guid": ")") + kMatGuid +
                   R"(", "roughness": 0.6, "clearcoat": 1, "clearcoat_roughness": 0.1, "sheen_color": [0.8, 0.7, 0.9],
  "sheen_roughness": 0.5, "anisotropy": -0.5, "anisotropy_rotation": 1.25, "transmission": 0.75, "ior": 1.33 })");
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(r.ok());
    AssetSystem as;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)).has_value());
    auto mh = as.load_sync<Material>(G(kMatGuid));
    REQUIRE(mh);
    const Span<const byte> payload = as.resolve(mh.value());
    REQUIRE(MaterialChunk::read(payload));
    CHECK(MaterialChunk::read(payload).value()->roughness == doctest::Approx(0.6f));
    const MaterialExt e = MaterialExt::of_material(payload);
    CHECK(e.any());
    CHECK(e.clearcoat == 1.0f);
    CHECK(e.clearcoat_roughness == doctest::Approx(0.1f));
    CHECK(e.sheen_color[2] == doctest::Approx(0.9f));
    CHECK(e.sheen_roughness == doctest::Approx(0.5f));
    CHECK(e.anisotropy == doctest::Approx(-0.5f));
    CHECK(e.anisotropy_rotation == doctest::Approx(1.25f));
    CHECK(e.transmission == doctest::Approx(0.75f));
    CHECK(e.ior == doctest::Approx(1.33f));
    as.release(AnyHandle(mh.value()));

    // The plain block alone: no extension follows, and it reads as all zero.
    MaterialChunk plain{};
    CHECK_FALSE(MaterialExt::of_material(as_bytes_of(plain)).any());

    write_file(root + "/assets/materials/hero.mcmat", std::string(R"({ "format": "mce-material", "version": 1, "guid": ")") + kMatGuid +
                                                          R"(", "clearcoat": 2, "ior": 0.5 })");
    const CookReport bad = svc.cook(root.c_str(), CookOptions{});
    CHECK(!bad.ok());
    CHECK(report_for(bad, kMatGuid)->status == AssetStatus::Failed);
}
