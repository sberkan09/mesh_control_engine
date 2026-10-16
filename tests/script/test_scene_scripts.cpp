// M-SCRIPT 1: .luau sources cook to CHNK_SCRIPT (errors at file:line:col), and scenes run them
// through kit::ScriptSystem components (mce/script/scene_scripts.h): bytecode only, in (order, ECS
// order) per phase, swapped when the set changes, deterministic.

#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "asset/cook_test_util.h"
#include "mce/asset/file_io.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/cook/cook.h"
#include "mce/ecs/transform.h"
#include "mce/kit/components.h"
#include "mce/script/compiler.h"
#include "mce/script/scene_scripts.h"

using namespace mce;
using namespace cook_test;

namespace {

constexpr const char* kPush = "5c000000-0000-4000-8000-000000000001";
constexpr const char* kDouble = "5c000000-0000-4000-8000-000000000002";
constexpr const char* kBroken = "5c000000-0000-4000-8000-000000000003";

// Moves every LocalTransform one unit along x per tick.
constexpr const char* kPushSource = R"(
function update()
    mce.each("LocalTransform", function(e)
        local p = mce.get(e, "LocalTransform", "position")
        mce.set(e, "LocalTransform", "position", p + vector.create(1, 0, 0))
    end)
end
)";

// Doubles x: run after push (x + 1) * 2, before it x * 2 + 1.
constexpr const char* kDoubleSource = R"(
function update()
    mce.each("LocalTransform", function(e)
        local p = mce.get(e, "LocalTransform", "position")
        mce.set(e, "LocalTransform", "position", vector.create(p.x * 2, p.y, p.z))
    end)
end
)";

std::string report(const cook::CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

struct Cooked {
    std::string dir;
    asset::AssetSystem assets;
};

void cook_scripts(Cooked& c) {
    const std::string root = fresh_dir("scene_scripts");
    write_file(root + "/assets/scripts/push.luau", kPushSource);
    write_file(root + "/assets/scripts/push.luau.mceasset", sidecar(kPush, "script"));
    write_file(root + "/assets/scripts/double.luau", kDoubleSource);
    write_file(root + "/assets/scripts/double.luau.mceasset", sidecar(kDouble, "script"));
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(report(r));
    REQUIRE(r.ok());
    c.dir = root + "/cooked";
    const StringView roots[] = {StringView(c.dir.c_str())};
    REQUIRE(c.assets.mount(Span<const StringView>(roots, 1)));
}

asset::Guid guid(const char* s) { return asset::parse_guid(StringView(s)).value(); }

Entity add_script(World& w, const char* g, i32 order, kit::ScriptPhase phase = kit::ScriptPhase::Pre) {
    kit::ScriptSystem s;
    s.script.guid = guid(g);
    s.order = order;
    s.phase = phase;
    return w.spawn_with(s);
}

f32 x_of(World& w, Entity e) { return w.try_get<LocalTransform>(e)->position.x; }

}  // namespace

TEST_CASE("scene scripts: .luau cooks to CHNK_SCRIPT bytecode named by its path") {
    Cooked c;
    cook_scripts(c);
    const asset::AssetKey key = c.assets.find(guid(kPush));
    REQUIRE(key != asset::kInvalidAssetKey);
    CHECK(c.assets.info(key).type == asset::ChunkType::Script);
    auto h = c.assets.load_sync<asset::Script>(key);
    REQUIRE(h);
    auto v = asset::ScriptView::read(c.assets.resolve(h.value()));
    REQUIRE(v);
    CHECK(v.value().name == StringView("scripts/push.luau"));
    CHECK(v.value().bytecode.size() > 8);
    c.assets.release(asset::AnyHandle(h.value()));
}

TEST_CASE("scene scripts: a syntax error fails the cook at file:line:col") {
    const std::string root = fresh_dir("scene_scripts_broken");
    write_file(root + "/assets/scripts/broken.luau", "function update()\n  local x = = 1\nend\n");
    write_file(root + "/assets/scripts/broken.luau.mceasset", sidecar(kBroken, "script"));
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    CHECK(!r.ok());
    const std::string text = report(r);
    INFO(text);
    CHECK(text.find("broken.luau:2:13") != std::string::npos);

    script::CompileError where;
    auto bad = script::compile("broken.luau", "function update()\n  local x = = 1\nend\n", &where);
    REQUIRE(!bad);
    CHECK(where.line == 2);
    CHECK(where.col == 13);
    CHECK(std::strstr(bad.error().path.c_str(), "broken.luau:2:13:") != nullptr);
}

TEST_CASE("scene scripts: ScriptSystems run by phase and order, and swap when the set changes") {
    Cooked c;
    cook_scripts(c);
    World w;
    w.register_components<LocalTransform, kit::ScriptSystem>();
    const Entity mover = w.spawn_with(LocalTransform{});
    // Listed double first, but push has the lower order: (0 + 1) * 2 = 2 after one tick.
    const Entity dbl = add_script(w, kDouble, 1);
    const Entity push = add_script(w, kPush, 0);
    script::install_scene_scripts(w, &c.assets);
    w.tick(1.0 / 60, 1);
    CHECK(x_of(w, mover) == doctest::Approx(2.0f));
    REQUIRE(script::scene_script_host(w));
    CHECK(script::scene_script_host(w)->scripts().size() == 2);
    CHECK(script::scene_script_host(w)->scripts()[0].name.view() == StringView("scripts/push.luau"));

    // Push moves to Post: double runs before physics, push after: 2 * 2 + 1 = 5.
    w.try_get_mut<kit::ScriptSystem>(push)->phase = kit::ScriptPhase::Post;
    w.tick(1.0 / 60, 2);
    CHECK(x_of(w, mover) == doctest::Approx(5.0f));

    // Disabled and removed scripts stop: only push is left (5 + 1).
    w.try_get_mut<kit::ScriptSystem>(dbl)->enabled = false;
    w.tick(1.0 / 60, 3);
    CHECK(x_of(w, mover) == doctest::Approx(6.0f));
    w.destroy(push);
    w.tick(1.0 / 60, 4);
    w.tick(1.0 / 60, 5);
    CHECK(x_of(w, mover) == doctest::Approx(6.0f));
    CHECK(script::scene_script_host(w) == nullptr);
}

TEST_CASE("scene scripts: a script that is not cooked is skipped, the others run") {
    Cooked c;
    cook_scripts(c);
    World w;
    w.register_components<LocalTransform, kit::ScriptSystem>();
    const Entity mover = w.spawn_with(LocalTransform{});
    add_script(w, "5c000000-0000-4000-8000-0000000000ff", 0);
    add_script(w, kPush, 1);
    script::install_scene_scripts(w, &c.assets);
    w.tick(1.0 / 60, 1);
    CHECK(x_of(w, mover) == doctest::Approx(1.0f));
}

TEST_CASE("scene scripts: the same scene gives the same state hash (FR-RUN-040)") {
    Cooked c;
    cook_scripts(c);
    auto run = [&]() {
        World w;
        w.register_components<LocalTransform, kit::ScriptSystem>();
        for (int i = 0; i < 8; ++i) {
            LocalTransform t;
            t.position = {static_cast<f32>(i) * 0.25f, 0, 0};
            w.spawn_with(t);
        }
        add_script(w, kPush, 0);
        add_script(w, kDouble, 1, kit::ScriptPhase::Post);
        script::install_scene_scripts(w, &c.assets);
        for (u32 t = 1; t <= 6; ++t) w.tick(1.0 / 60, t);
        return w.state_hash();
    };
    CHECK(run() == run());
}

TEST_CASE("scene scripts: a script recooked while the scene runs takes effect on the next tick") {
    Cooked c;
    cook_scripts(c);
    World w;
    w.register_components<LocalTransform, kit::ScriptSystem>();
    const Entity mover = w.spawn_with(LocalTransform{});
    add_script(w, kPush, 0);
    script::install_scene_scripts(w, &c.assets);
    w.tick(1.0 / 60, 1);
    CHECK(x_of(w, mover) == doctest::Approx(1.0f));

    // What the editor's hot reload does: edit the source, recook, swap the container in place.
    const std::string root = c.dir.substr(0, c.dir.size() - std::string("/cooked").size());
    write_file(root + "/assets/scripts/push.luau", std::string(kPushSource).replace(std::string(kPushSource).find("1, 0, 0"), 7, "10, 0, 0"));
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    REQUIRE(r.ok());
    u32 swapped = 0;
    for (const cook::AssetReport& a : r.assets) {
        if (a.status != cook::AssetStatus::Cooked) continue;
        const FixedString<40> hex = asset::to_hex(a.guid);
        char name[128];
        std::snprintf(name, sizeof(name), "/cooked/%.2s/%s/%016llx.mce", hex.c_str(), hex.c_str(), static_cast<unsigned long long>(a.output_hash));
        Array<byte> bytes;
        REQUIRE(asset::read_file(StringView((root + name).c_str()), bytes));
        REQUIRE(c.assets.commit_reload(a.guid, bytes.span()));
        ++swapped;
    }
    CHECK(swapped == 1);
    w.tick(1.0 / 60, 2);
    CHECK(x_of(w, mover) == doctest::Approx(11.0f));  // 1 + the recooked script's 10
}
