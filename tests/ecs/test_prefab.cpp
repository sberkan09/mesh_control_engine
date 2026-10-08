// Prefabs (v0.5, FR-ENT-070): .mcprefab -> cook (nested instances flattened) -> CHNK_PREFAB ->
// instantiate = bulk row copy + handle remap + overrides; scenes holding prefab instances load
// through the same path (FR-ENT-060). No runtime inheritance: the runtime only ever sees rows.

#include <doctest/doctest.h>

#include <cstring>

#include "asset/cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/prefab.h"

using namespace mce;
using namespace mce::asset;
using namespace cook_test;
using cook::CookOptions;
using cook::CookReport;
using cook::CookService;

namespace pftest {

struct Health {
    f32 hp = 10.0f;
    Entity target;
};
MCE_COMPONENT(Health, hp, target)

struct Skin {
    AssetRef<Texture> texture;
    u32 frame = 0;
};
MCE_COMPONENT(Skin, texture, frame)

struct Loot {  // never registered with the World in the error test
    u32 gold = 0;
};
MCE_COMPONENT(Loot, gold)

}  // namespace pftest

using pftest::Health;
using pftest::Skin;
using mce::math::vec3;

namespace {

constexpr const char* kTex = "b2b2b2b2-0000-4000-8000-000000000001";
constexpr const char* kTurret = "b2b2b2b2-0000-4000-8000-000000000002";
constexpr const char* kSquad = "b2b2b2b2-0000-4000-8000-000000000003";
constexpr const char* kLevel = "b2b2b2b2-0000-4000-8000-000000000004";
constexpr const char* kSquadManual = "b2b2b2b2-0000-4000-8000-000000000005";

Guid G(const char* s) { return parse_guid(s).value(); }

std::string turret(int hp = 50) {
    return std::string(R"({ "format": "mce-prefab", "version": 1, "guid": ")") + kTurret + R"(", "name": "turret",
  "entities": [
    { "name": "Turret", "components": {
        "LocalTransform": { "position": [0, 1, 0] },
        "Health": { "hp": )" + std::to_string(hp) + R"(, "target": "$1" } } },
    { "name": "Barrel", "components": {
        "LocalTransform": { "position": [1, 0, 0] },
        "ChildOf": { "parent": "$0" },
        "Skin": { "texture": "guid:)" + kTex + R"(", "frame": 2 } } }
  ] })";
}

// Two turret instances under a root; the second overrides hp and the first moves.
std::string squad() {
    return std::string(R"({ "format": "mce-prefab", "version": 1, "guid": ")") + kSquad + R"(", "name": "squad",
  "entities": [
    { "name": "Squad", "components": { "LocalTransform": { "position": [0, 0, 0] } } },
    { "prefab": "guid:)" + kTurret + R"(", "components": {
        "LocalTransform": { "position": [5, 0, 0] }, "ChildOf": { "parent": "$0" } } },
    { "name": "T2", "prefab": "guid:)" + kTurret + R"(", "components": {
        "Health": { "hp": 99 }, "ChildOf": { "parent": "$0" } } }
  ] })";
}

// The same content as squad(), composed by hand: flattening must produce identical rows.
std::string squad_manual(int hp = 50) {
    const std::string h = std::to_string(hp);
    return std::string(R"({ "format": "mce-prefab", "version": 1, "guid": ")") + kSquadManual + R"(", "name": "squad",
  "entities": [
    { "name": "Squad", "components": { "LocalTransform": { "position": [0, 0, 0] } } },
    { "name": "Turret", "components": {
        "LocalTransform": { "position": [5, 0, 0] }, "ChildOf": { "parent": "$0" },
        "Health": { "hp": )" + h + R"(, "target": "$2" } } },
    { "name": "Barrel", "components": {
        "LocalTransform": { "position": [1, 0, 0] }, "ChildOf": { "parent": "$1" },
        "Skin": { "texture": "guid:)" + kTex + R"(", "frame": 2 } } },
    { "name": "T2", "components": {
        "LocalTransform": { "position": [0, 1, 0] }, "ChildOf": { "parent": "$0" },
        "Health": { "hp": 99, "target": "$4" } } },
    { "name": "Barrel", "components": {
        "LocalTransform": { "position": [1, 0, 0] }, "ChildOf": { "parent": "$3" },
        "Skin": { "texture": "guid:)" + kTex + R"(", "frame": 2 } } }
  ] })";
}

std::string level() {
    return std::string(R"({ "format": "mce-scene", "version": 1, "guid": ")") + kLevel + R"(", "name": "level",
  "entities": [
    { "name": "Watcher", "components": { "Health": { "hp": 1, "target": "$1" } } },
    { "prefab": "guid:)" + kSquad + R"(", "components": { "LocalTransform": { "position": [100, 0, 0] } } }
  ] })";
}

std::string make_project(const char* name) {
    const std::string root = fresh_dir(name);
    const std::string a = root + "/assets";
    write_file(a + "/textures/skin.png", make_gradient_png(4, 4));
    write_file(a + "/textures/skin.png.mceasset", sidecar(kTex, "texture"));
    write_file(a + "/prefabs/turret.mcprefab", turret());
    write_file(a + "/prefabs/squad.mcprefab", squad());
    write_file(a + "/prefabs/squad_manual.mcprefab", squad_manual());
    write_file(a + "/scenes/level.mcscene", level());
    return root;
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

bool has_diag(const CookReport& r, Errc code, const char* text) {
    for (const auto& d : r.diags.all())
        if (d.code == code && std::strstr(d.message.c_str(), text)) return true;
    return false;
}

const cook::AssetReport* report_for(const CookReport& r, const char* guid) {
    for (const auto& a : r.assets)
        if (a.guid == G(guid)) return &a;
    return nullptr;
}

struct Mounted {
    AssetSystem as;
    explicit Mounted(const std::string& root) {
        const std::string cooked = root + "/cooked";
        const StringView roots[] = {cooked};
        REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    }
    template <class T>
    SceneView view(const char* guid) {
        auto h = as.load_sync<T>(G(guid));
        REQUIRE(h);
        auto v = SceneView::read(as.resolve(h.value()));
        REQUIRE(v);
        return v.value();
    }
};

bool stamp_texture(void* ctx, const SceneAssetRef& ref, void* field) {
    auto* as = static_cast<AssetSystem*>(ctx);
    auto h = as->load_sync<Texture>(ref.guid());
    if (!h) return false;
    static_cast<AssetRef<Texture>*>(field)->handle = h.value();
    return true;
}

void setup(World& w) {
    w.register_components<Health, Skin>();
    ecs::install_hierarchy(w);
}

vec3 world_pos(World& w, Entity e) { return w.try_get<WorldTransform>(e)->position; }

}  // namespace

TEST_CASE("prefab: cook flattens nested instances; instantiate remaps handles and applies overrides") {
    const std::string root = make_project("prefab_basic");
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());
    CHECK(r.cooked == 5);
    Mounted m(root);

    const SceneView tv = m.view<Prefab>(kTurret);
    CHECK(tv.header->entity_count == 2);
    const SceneView sv = m.view<Prefab>(kSquad);
    REQUIRE(sv.header->entity_count == 5);  // root + 2 x (turret + barrel)
    CHECK(sv.entity_name(1) == "Turret");    // unnamed instance keeps the prefab root's name
    CHECK(sv.entity_name(3) == "T2");

    World w;
    setup(w);
    Array<Entity> es;
    ecs::SpawnParams p;
    p.resolve_asset = &stamp_texture;
    p.resolve_ctx = &m.as;
    auto n = ecs::instantiate(w, sv, p, &es);
    REQUIRE(n);
    CHECK(n.value() == 5);
    REQUIRE(es.size() == 5);
    ecs::propagate_transforms(w);

    // Intra-prefab references point at this instance's own entities.
    CHECK(w.try_get<Health>(es[1])->target == es[2]);
    CHECK(w.try_get<Health>(es[3])->target == es[4]);
    CHECK(w.try_get<Health>(es[1])->hp == 50.0f);
    CHECK(w.try_get<Health>(es[3])->hp == 99.0f);  // instance override
    CHECK(ecs::parent_of(w, es[2]) == es[1]);
    CHECK(ecs::parent_of(w, es[1]) == es[0]);
    CHECK(math::near(world_pos(w, es[2]), {6, 0, 0}));  // 5 (override) + 1 (barrel)
    CHECK(math::near(world_pos(w, es[4]), {1, 1, 0}));  // prefab default (0,1,0) + 1
    // Asset references were stamped through the caller's resolver.
    const Skin* skin = w.try_get<Skin>(es[2]);
    REQUIRE(skin);
    CHECK(skin->texture.guid == G(kTex));
    CHECK(skin->texture.handle.valid());
    CHECK(skin->frame == 2);
}

TEST_CASE("prefab: nested flatten equals the hand-composed prefab (row-identical)") {
    const std::string root = make_project("prefab_flatten_eq");
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());
    Mounted m(root);
    World a, b;
    setup(a);
    setup(b);
    REQUIRE(ecs::instantiate(a, m.view<Prefab>(kSquad)));
    REQUIRE(ecs::instantiate(b, m.view<Prefab>(kSquadManual)));
    ecs::propagate_transforms(a);
    ecs::propagate_transforms(b);
    CHECK(a.entity_count() == 5);
    CHECK(a.state_hash() == b.state_hash());
}

TEST_CASE("prefab: replicas, a parent and reflection-path overrides") {
    const std::string root = make_project("prefab_replicas");
    CookService svc;
    REQUIRE(svc.cook(root.c_str(), CookOptions{}).ok());
    Mounted m(root);
    World w;
    setup(w);
    const Entity holder = w.spawn_with(LocalTransform{{0, 0, 10}, {}, {1, 1, 1}});

    const ecs::PrefabOverride ov[] = {
        ecs::PrefabOverride::of("LocalTransform.position", math::vec3{3, 0, 0}),
        ecs::PrefabOverride::of("Health.hp", 7.0f),
        ecs::PrefabOverride::of("Skin.frame", u32(9), 1),
    };
    ecs::SpawnParams p;
    p.overrides = Span<const ecs::PrefabOverride>(ov, 3);
    p.parent = holder;
    p.count = 3;
    Array<Entity> es;
    auto n = ecs::instantiate(w, m.view<Prefab>(kTurret), p, &es);
    REQUIRE(n);
    CHECK(n.value() == 6);
    ecs::propagate_transforms(w);
    for (u32 k = 0; k < 3; ++k) {
        const Entity rt = es[k * 2], barrel = es[k * 2 + 1];
        CHECK(w.try_get<Health>(rt)->hp == 7.0f);
        CHECK(w.try_get<Health>(rt)->target == barrel);
        CHECK(w.try_get<Skin>(barrel)->frame == 9);
        CHECK(ecs::parent_of(w, rt) == holder);   // roots attach to the parent
        CHECK(ecs::parent_of(w, barrel) == rt);   // non-roots keep their template parent
        CHECK(math::near(world_pos(w, barrel), {4, 0, 10}));
        CHECK_FALSE(w.try_get<Skin>(barrel)->texture.handle.valid());  // no resolver: GUID only
    }
    CHECK(ecs::children_of(w, holder).size() == 3);
    // The parent's subtree goes with it.
    w.destroy(holder);
    CHECK(w.entity_count() == 0);
}

TEST_CASE("prefab: a scene with prefab instances loads through the same path") {
    const std::string root = make_project("prefab_scene");
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());
    Mounted m(root);
    const SceneView lv = m.view<Scene>(kLevel);
    REQUIRE(lv.header->entity_count == 6);
    World w;
    setup(w);
    Array<Entity> es;
    REQUIRE(ecs::load_scene(w, lv, &es));
    ecs::propagate_transforms(w);
    CHECK(w.try_get<Health>(es[0])->target == es[1]);       // "$1" = the instance root
    CHECK(math::near(world_pos(w, es[3]), {106, 0, 0}));     // 100 + 5 + 1
}

TEST_CASE("prefab: editing a prefab re-cooks its users; overrides survive") {
    const std::string root = make_project("prefab_edit");
    CookService svc;
    REQUIRE(svc.cook(root.c_str(), CookOptions{}).ok());
    write_file(root + "/assets/prefabs/turret.mcprefab", turret(60));
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());
    for (const char* g : {kTurret, kSquad, kLevel}) {
        const cook::AssetReport* a = report_for(r, g);
        REQUIRE(a);
        CHECK(a->status == cook::AssetStatus::Cooked);
    }
    CHECK(report_for(r, kSquadManual)->status == cook::AssetStatus::UpToDate);
    Mounted m(root);
    World w;
    setup(w);
    Array<Entity> es;
    REQUIRE(ecs::load_scene(w, m.view<Scene>(kLevel), &es));
    CHECK(w.try_get<Health>(es[2])->hp == 60.0f);  // new prefab value
    CHECK(w.try_get<Health>(es[4])->hp == 99.0f);  // override kept
}

TEST_CASE("prefab: cook errors — unknown prefab, cycles, empty prefabs") {
    SUBCASE("unknown GUID") {
        const std::string root = make_project("prefab_err_unknown");
        write_file(root + "/assets/scenes/bad.mcscene", R"({ "format": "mce-scene", "version": 1,
  "guid": "b2b2b2b2-0000-4000-8000-0000000000a1", "entities": [
    { "prefab": "guid:b2b2b2b2-0000-4000-8000-0000000000ff" } ] })");
        CookService svc;
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        CHECK_FALSE(r.ok());
        CHECK(r.diags.error_count() > 0);
    }
    SUBCASE("cycle") {
        const std::string root = make_project("prefab_err_cycle");
        write_file(root + "/assets/prefabs/a.mcprefab", R"({ "format": "mce-prefab", "version": 1,
  "guid": "b2b2b2b2-0000-4000-8000-0000000000a2", "entities": [
    { "prefab": "guid:b2b2b2b2-0000-4000-8000-0000000000a3" } ] })");
        write_file(root + "/assets/prefabs/b.mcprefab", R"({ "format": "mce-prefab", "version": 1,
  "guid": "b2b2b2b2-0000-4000-8000-0000000000a3", "entities": [
    { "prefab": "guid:b2b2b2b2-0000-4000-8000-0000000000a2" } ] })");
        CookService svc;
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        CHECK_FALSE(r.ok());
        CHECK(r.diags.error_count() > 0);
    }
    SUBCASE("empty prefab") {
        const std::string root = make_project("prefab_err_empty");
        write_file(root + "/assets/prefabs/e.mcprefab", R"({ "format": "mce-prefab", "version": 1,
  "guid": "b2b2b2b2-0000-4000-8000-0000000000a4", "entities": [] })");
        CookService svc;
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        CHECK_FALSE(r.ok());
        CHECK(has_diag(r, Errc::InvalidArgument, "at least one entity"));
    }
}

TEST_CASE("prefab: runtime errors spawn nothing") {
    const std::string root = make_project("prefab_rt_err");
    CookService svc;
    REQUIRE(svc.cook(root.c_str(), CookOptions{}).ok());
    Mounted m(root);
    const SceneView tv = m.view<Prefab>(kTurret);

    World unregistered;  // Health/Skin never registered
    ecs::install_hierarchy(unregistered);
    auto r1 = ecs::instantiate(unregistered, tv);
    REQUIRE_FALSE(r1);
    CHECK(r1.error().code == Errc::NotFound);
    CHECK(unregistered.entity_count() == 0);

    World w;
    setup(w);
    const ecs::PrefabOverride bad_path[] = {ecs::PrefabOverride::of("Health.nope", 1.0f)};
    const ecs::PrefabOverride bad_type[] = {ecs::PrefabOverride::of("Health.hp", u32(1))};
    const ecs::PrefabOverride bad_comp[] = {ecs::PrefabOverride::of("Loot.gold", u32(1))};  // reflected, unregistered
    const ecs::PrefabOverride bad_entity[] = {ecs::PrefabOverride::of("Health.hp", 1.0f, 7)};
    for (Span<const ecs::PrefabOverride> ov : {Span<const ecs::PrefabOverride>(bad_path), Span<const ecs::PrefabOverride>(bad_type),
                                               Span<const ecs::PrefabOverride>(bad_comp), Span<const ecs::PrefabOverride>(bad_entity)}) {
        ecs::SpawnParams p;
        p.overrides = ov;
        CHECK_FALSE(ecs::instantiate(w, tv, p));
    }
    CHECK(w.entity_count() == 0);
    // A failing asset resolver also aborts before anything is spawned.
    ecs::SpawnParams p;
    p.resolve_asset = [](void*, const SceneAssetRef&, void*) { return false; };
    auto r2 = ecs::instantiate(w, tv, p);
    REQUIRE_FALSE(r2);
    CHECK(r2.error().code == Errc::MissingRef);
    CHECK(w.entity_count() == 0);
}

TEST_CASE("prefab: instantiation inside a phase goes through Commands") {
    const std::string root = make_project("prefab_in_phase");
    CookService svc;
    REQUIRE(svc.cook(root.c_str(), CookOptions{}).ok());
    Mounted m(root);
    World w;
    setup(w);
    struct Spawner {
        SceneView view;
        u32 spawned = 0;
        bool alive_at_once = false;
    };
    w.set_resource(Spawner{m.view<Prefab>(kTurret)});
    w.add_system(Phase::FixedUpdate_Pre, "spawner", [](Frame& f) {
        Spawner& s = f.resource<Spawner>();
        Array<Entity> es;
        const ecs::PrefabOverride ov[] = {ecs::PrefabOverride::of("LocalTransform.position", math::vec3{f32(f.tick), 0, 0})};
        ecs::SpawnParams p;
        p.overrides = Span<const ecs::PrefabOverride>(ov, 1);
        auto n = ecs::instantiate(f.world(), s.view, p, &es);
        s.spawned += n ? n.value() : 0;
        s.alive_at_once = f.world().alive(es[0]) && !f.world().has<Health>(es[0]);  // handles first, rows at sync
    });
    w.tick(1.0 / 60, 0);
    w.tick(1.0 / 60, 1);
    CHECK(w.resource<Spawner>().spawned == 4);
    CHECK(w.resource<Spawner>().alive_at_once);
    CHECK(w.query<const Health&>().count() == 2);
    CHECK(w.query<const WorldTransform&>().with<ChildOf>().count() == 2);  // propagated the same tick
}
