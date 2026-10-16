// View and local-player tools (M-VIEWS step 6): view_list, view_add, player_list and player_setup.
//
// They read and write the open scene's M-VIEWS components (kit/views.h, kit/render_textures.h,
// kit/coop.h) the way an agent would otherwise have to by hand with entity_create: view_list shows
// every camera view, split-screen setting and render texture with the problems it finds (a texture
// nothing draws into, a view into an entity that is no texture ...); view_add makes a render texture,
// the camera that fills it and, on request, a surface that shows it; player_list reports the co-op
// game, each player's spawn, camera and input map (read from the project's .mcinput files);
// player_setup builds a whole co-op rig in one call. render_frame's `view` argument (tools_render.cpp)
// looks at one player's view or one texture's camera.

#include <algorithm>

#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/kit/cinematic.h"
#include "mce/kit/components.h"
#include "mce/kit/coop.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/render_textures.h"
#include "mce/kit/views.h"
#include "tool_args.h"
#include "tools.h"

namespace mce::mcp {

namespace {

using editor::DocEntity;
using editor::EditorScene;
using editor::EntityId;

template <class T>
const T* comp(const DocEntity& e) {
    const editor::DocComponent* c = e.find(refl::type_of<T>());
    return c ? static_cast<const T*>(c->data()) : nullptr;
}

template <class T>
void put(EditorScene& s, EntityId id, const T& value) {
    s.add_component(id, refl::type_of<T>());
    s.set(id, value);
}

void write_ref(JsonWriter& w, const char* key, const editor::SceneDocument& doc, EntityId id) {
    w.key(key);
    const DocEntity* e = id == editor::kNoEntity ? nullptr : doc.find(id);
    if (!e) {
        w.null();
        return;
    }
    w.begin_object().key("id").num_u(e->id).key("name").str(e->name.view()).end_object();
}

// A name that does not clash with an entity of the document: "Minimap", then "Minimap 2" ...
cook::String unique_name(const editor::SceneDocument& doc, StringView base) {
    for (u32 n = 1; n < 1000; ++n) {
        cook::String name(base);
        if (n > 1) cook::appendf(name, " %u", n);
        bool taken = false;
        for (const DocEntity& e : doc.entities()) taken = taken || e.name.view() == name.view();
        if (!taken) return name;
    }
    return cook::String(base);
}

cook::String cat(StringView a, StringView b, StringView c = {}) {
    cook::String out(a);
    out += b;
    out += c;
    return out;
}

bool numbers(const cook::JsonValue& v, usize n, f64* out) {
    if (!v.is_array() || v.size() != n) return false;
    for (u32 i = 0; i < n; ++i) {
        if (!v.at(i).is_number()) return false;
        out[i] = v.at(i).as_number();
    }
    return true;
}

bool arg_number(ToolCall& c, const char* name, f64& out, f64 lo, f64 hi) {
    const cook::JsonValue v = arg(c, name);
    if (!v.valid()) return true;
    if (!v.is_number() || v.as_number() < lo || v.as_number() > hi) {
        cook::String expected;
        cook::appendf(expected, "a number in [%g, %g]", lo, hi);
        return arg_missing(c, name, expected.c_str());
    }
    out = v.as_number();
    return true;
}

struct Problems {
    Array<cook::String> list;
    void add(StringView text) { list.push_back(cook::String(text)); }
    void write(JsonWriter& w) const {
        w.key("problems").begin_array();
        for (const cook::String& p : list) w.str(p.view());
        w.end_array();
    }
};

// ---- view_list -------------------------------------------------------------------------------------

bool view_list(ToolCall& c) {
    const editor::SceneDocument& doc = scene(c).doc();
    Problems problems;
    JsonWriter& w = c.out;
    w.begin_object();
    w.key("scene").str(c.server.scene_file().rel.empty() ? StringView("(unsaved)") : c.server.scene_file().rel.view());

    const kit::SplitScreen* split = nullptr;
    for (const DocEntity& e : doc.entities())
        if (!split) split = comp<kit::SplitScreen>(e);
    static const char* const kLayouts[] = {"auto", "columns", "rows", "grid"};
    w.key("split_screen");
    if (split) {
        w.begin_object().key("layout").str(kLayouts[std::min<u32>(static_cast<u32>(split->layout), 3)]).key("divider").num_u(split->divider).end_object();
    } else {
        w.null();
    }

    u32 split_used = 0;
    u32 window_views = 0;
    w.key("views").begin_array();
    for (const DocEntity& e : doc.entities()) {
        const kit::CameraView* v = comp<kit::CameraView>(e);
        if (!v) continue;
        const bool is3d = comp<kit::Camera3DComponent>(e) != nullptr;
        if (!is3d && !comp<kit::Camera2DComponent>(e)) problems.add(cat("camera view on '", e.name.view(), "' has no Camera2DComponent or Camera3DComponent: it draws nothing"));
        w.begin_object().key("id").num_u(e.id).key("name").str(e.name.view());
        w.key("kind").str(is3d ? "camera3d" : "camera2d");
        w.key("enabled").boolean(v->enabled != 0);
        w.key("order").num_i(v->order);
        w.key("viewport").begin_array().num_f(v->viewport.x).num_f(v->viewport.y).num_f(v->viewport.z).num_f(v->viewport.w).end_array();
        write_ref(w, "target", doc, editor::doc_entity_id(v->target));
        w.key("split").num_u(v->split);
        if (const kit::PlayerCamera* pc = comp<kit::PlayerCamera>(e)) w.key("player_camera").num_u(pc->player);
        w.end_object();
        if (v->target.is_null()) {
            ++window_views;
            if (v->split >= 1 && v->split <= 4) split_used |= 1u << v->split;
        } else {
            const DocEntity* t = doc.find(editor::doc_entity_id(v->target));
            if (!t || !comp<kit::RenderTexture>(*t))
                problems.add(cat("'", e.name.view(), "' draws into an entity that has no RenderTexture (CameraView.target)"));
            if (v->split != 0) problems.add(cat("'", e.name.view(), "' has both a target and a split: split only applies to the window"));
        }
        if (v->split >= 1 && v->split <= 4 && v->enabled == 0 && !comp<kit::PlayerCamera>(e))
            problems.add(cat("'", e.name.view(), "' is a split-screen camera that starts disabled and has no PlayerCamera to enable it"));
    }
    w.end_array();
    (void)window_views;
    if (split_used != 0 && !split) problems.add("cameras use split but the scene has no SplitScreen: the layout is automatic with no divider (add one to choose)");

    w.key("textures").begin_array();
    for (const DocEntity& e : doc.entities()) {
        const kit::RenderTexture* rt = comp<kit::RenderTexture>(e);
        if (!rt) continue;
        w.begin_object().key("id").num_u(e.id).key("name").str(e.name.view());
        w.key("width").num_u(rt->width).key("height").num_u(rt->height).key("update_hz").num_f(rt->update_hz).key("enabled").boolean(rt->enabled != 0);
        if (!rt->ui_image.empty()) w.key("ui_image").str(rt->ui_image.view());
        u32 cameras = 0;
        w.key("cameras").begin_array();
        for (const DocEntity& o : doc.entities()) {
            const kit::CameraView* v = comp<kit::CameraView>(o);
            if (v && editor::doc_entity_id(v->target) == e.id) {
                w.str(o.name.view());
                ++cameras;
            }
        }
        w.end_array();
        u32 shown = 0;
        w.key("shown_on").begin_array();
        for (const DocEntity& o : doc.entities()) {
            const kit::ShowRenderTexture* s = comp<kit::ShowRenderTexture>(o);
            if (s && editor::doc_entity_id(s->source) == e.id) {
                w.str(o.name.view());
                ++shown;
            }
        }
        w.end_array().end_object();
        if (cameras == 0) problems.add(cat("render texture '", e.name.view(), "' has no camera drawing into it: it stays black (view_add makes one)"));
        if (shown == 0 && rt->ui_image.empty()) problems.add(cat("render texture '", e.name.view(), "' is shown nowhere (ShowRenderTexture on a Sprite or MeshRef, or ui_image)"));
    }
    w.end_array();
    for (const DocEntity& e : doc.entities()) {
        const kit::ShowRenderTexture* s = comp<kit::ShowRenderTexture>(e);
        if (!s) continue;
        const DocEntity* t = s->source.is_null() ? nullptr : doc.find(editor::doc_entity_id(s->source));
        if (!t || !comp<kit::RenderTexture>(*t)) problems.add(cat("'", e.name.view(), "' shows a source that has no RenderTexture (ShowRenderTexture.source)"));
    }
    problems.write(w);
    w.end_object();
    return true;
}

// ---- view_add --------------------------------------------------------------------------------------

bool view_add(ToolCall& c) {
    StringView name, kind = "2d";
    u64 width = 256, height = 256;
    f64 hz = 0.0;
    if (!arg_string(c, "name", name, true) || !arg_string(c, "camera", kind, false) || !arg_uint(c, "width", width, 1, 4096, false) ||
        !arg_uint(c, "height", height, 1, 4096, false) || !arg_number(c, "update_hz", hz, 0, 240))
        return false;
    if (kind != "2d" && kind != "3d") return arg_missing(c, "camera", "\"2d\" or \"3d\"");
    if (name.size() > 40) return fail(c, "argument 'name' must be at most 40 bytes");
    f64 pos[3] = {0, 0, kind == "3d" ? 10.0 : 0.0}, look[3] = {0, 0, 0}, fit[2] = {0, 0};
    if (const cook::JsonValue p = arg(c, "position"); p.valid() && !numbers(p, 3, pos)) return arg_missing(c, "position", "[x, y, z]");
    if (const cook::JsonValue f = arg(c, "fit"); f.valid() && !numbers(f, 2, fit)) return arg_missing(c, "fit", "[width, height] in metres (2D)");
    if (const cook::JsonValue t = arg(c, "look_at"); t.valid() && !numbers(t, 3, look)) return arg_missing(c, "look_at", "[x, y, z] (3D)");
    EntityId show_on = editor::kNoEntity;
    if (arg(c, "show_on").valid()) {
        if (!arg_entity(c, "show_on", show_on)) return false;
        const DocEntity* t = scene(c).doc().find(show_on);
        if (!t->find(refl::type_of<kit::Sprite>()) && !t->find(refl::type_of<kit::MeshRef>()))
            return fail(c, "argument 'show_on': the entity needs a Sprite or a MeshRef to show the texture");
    }

    EditorScene& s = scene(c);
    const cook::String tex_name = unique_name(s.doc(), name);
    const cook::String cam_name = unique_name(s.doc(), cat(name, " camera"));
    const EntityId tex = s.create_entity(tex_name.view(), false);
    kit::RenderTexture rt;
    rt.width = static_cast<u32>(width);
    rt.height = static_cast<u32>(height);
    rt.update_hz = static_cast<f32>(hz);
    put(s, tex, rt);
    const EntityId cam = s.create_entity(cam_name.view(), true);
    LocalTransform lt;
    lt.position = math::vec3{static_cast<f32>(pos[0]), static_cast<f32>(pos[1]), static_cast<f32>(pos[2])};
    if (kind == "3d") {
        const math::vec3 fwd{static_cast<f32>(look[0] - pos[0]), static_cast<f32>(look[1] - pos[1]), static_cast<f32>(look[2] - pos[2])};
        lt.rotation = kit::look_rotation(fwd);
        kit::Camera3DComponent cc;
        cc.primary = 0;
        put(s, cam, cc);
    } else {
        kit::Camera2DComponent cc;
        cc.primary = false;
        cc.pixel_snap = false;
        cc.fit_width = static_cast<f32>(fit[0]);
        cc.fit_height = static_cast<f32>(fit[1]);
        put(s, cam, cc);
    }
    s.set(cam, lt);
    kit::CameraView cv;
    cv.target = editor::doc_entity_ref(tex);
    put(s, cam, cv);
    if (show_on != editor::kNoEntity) {
        kit::ShowRenderTexture sh;
        sh.source = editor::doc_entity_ref(tex);
        put(s, show_on, sh);
    }
    c.out.begin_object().key("texture").begin_object().key("id").num_u(tex).key("name").str(tex_name.view()).end_object();
    c.out.key("camera").begin_object().key("id").num_u(cam).key("name").str(cam_name.view()).end_object();
    if (show_on != editor::kNoEntity) c.out.key("shown_on").num_u(show_on);
    c.out.end_object();
    return true;
}

// ---- player_list -----------------------------------------------------------------------------------

struct InputMapInfo {
    cook::String asset;
    cook::String map;
    u32 player = 0;  // 0: no "player" key (the plain map, player 1's)
    u32 actions = 0;
    bool join = false;
};

void read_input_maps(ToolCall& c, Array<InputMapInfo>& out) {
    if (!c.server.session().project().is_open()) return;
    for (const editor::ProjectAsset& a : c.server.session().project().assets()) {
        if (a.path.size() < 9 || a.path.view().substr(a.path.size() - 8) != ".mcinput") continue;
        cook::String text;
        if (!cook::fs::read_text(cook::path_join(cook::path_join(c.server.session().project().root(), "assets").view(), a.path.view()).view(), text)) continue;
        cook::JsonDoc doc;
        cook::JsonError err;
        if (!doc.parse(text.view(), err)) continue;
        const cook::JsonValue maps = doc.root().get("maps");
        for (u32 i = 0; maps.valid() && maps.is_array() && i < maps.size(); ++i) {
            const cook::JsonValue m = maps.at(i);
            InputMapInfo info;
            info.asset = a.path;
            if (const cook::JsonValue n = m.get("name"); n.valid() && n.is_string()) info.map = cook::String(n.as_string());
            if (const cook::JsonValue p = m.get("player"); p.valid() && p.is_number()) info.player = static_cast<u32>(p.as_number());
            const cook::JsonValue acts = m.get("actions");
            for (u32 k = 0; acts.valid() && acts.is_array() && k < acts.size(); ++k) {
                ++info.actions;
                const cook::JsonValue an = acts.at(k).get("name");
                if (an.valid() && an.is_string() && an.as_string() == "Join") info.join = true;
            }
            out.push_back(static_cast<InputMapInfo&&>(info));
        }
    }
}

bool player_list(ToolCall& c) {
    const editor::SceneDocument& doc = scene(c).doc();
    Problems problems;
    JsonWriter& w = c.out;
    w.begin_object();
    const kit::CoopGame* game = nullptr;
    const DocEntity* game_entity = nullptr;
    for (const DocEntity& e : doc.entities())
        if (!game && (game = comp<kit::CoopGame>(e))) game_entity = &e;
    w.key("coop_game");
    if (game) {
        w.begin_object().key("entity").str(game_entity->name.view()).key("prefab").str(asset::to_string(game->player.guid).view());
        w.key("max_players").num_u(game->max_players).key("auto_join").num_u(game->auto_join).key("tint_players").boolean(game->tint_players);
        w.key("enabled").boolean(game->enabled).end_object();
        if (game->player.guid.is_null()) problems.add("CoopGame has no player prefab: players are bare coloured squares (set CoopGame.player)");
    } else {
        w.null();
        problems.add("the scene has no CoopGame: nothing spawns players (player_setup adds one)");
    }

    Array<InputMapInfo> maps;
    read_input_maps(c, maps);
    const u32 max_players = game ? std::min<u32>(std::max<u32>(game->max_players, 1), 4) : 4;
    w.key("players").begin_array();
    for (u32 p = 1; p <= 4; ++p) {
        w.begin_object().key("player").num_u(p);
        w.key("spawns").begin_array();
        u32 spawns = 0;
        for (const DocEntity& e : doc.entities())
            if (const kit::PlayerSpawn* sp = comp<kit::PlayerSpawn>(e); sp && (sp->player == p || sp->player == 0)) {
                w.str(e.name.view());
                ++spawns;
            }
        w.end_array();
        w.key("cameras").begin_array();
        u32 cams = 0;
        for (const DocEntity& e : doc.entities())
            if (const kit::PlayerCamera* pc = comp<kit::PlayerCamera>(e); pc && pc->player == p) {
                w.str(e.name.view());
                ++cams;
                const kit::CameraView* cv = comp<kit::CameraView>(e);
                if (!cv) problems.add(cat("PlayerCamera '", e.name.view(), "' has no CameraView: it moves but nothing draws it"));
                else if (cv->split != p && cv->target.is_null()) {
                    cook::String m("PlayerCamera '");
                    m += e.name.view();
                    cook::appendf(m, "' follows player %u but its CameraView.split is %u (give it the same number to fill that player's cell)", p, cv->split);
                    problems.add(m.view());
                }
            }
        w.end_array();
        w.key("input_maps").begin_array();
        u32 mapped = 0;
        for (const InputMapInfo& m : maps)
            if (m.player == p || (p == 1 && m.player == 0)) {
                w.begin_object().key("asset").str(m.asset.view()).key("map").str(m.map.view()).key("actions").num_u(m.actions).key("join").boolean(m.join).end_object();
                ++mapped;
            }
        w.end_array().end_object();
        if (p <= max_players && game) {
            cook::String m("player ");
            cook::appendf(m, "%u", p);
            if (spawns == 0) problems.add(cat(m.view(), " has no PlayerSpawn: they appear in a row at x = 2 * (player - 1)"));
            if (cams == 0 && p > 1) problems.add(cat(m.view(), " has no PlayerCamera: they share whatever camera draws the window"));
            if (mapped == 0 && p > 1) problems.add(cat(m.view(), " has no input map with \"player\": (a .mcinput map per player; samples/09_coop shows the pattern)"));
        }
    }
    w.end_array();
    problems.write(w);
    w.end_object();
    return true;
}

// ---- player_setup ----------------------------------------------------------------------------------

bool player_setup(ToolCall& c) {
    u64 players = 2;
    f64 spacing = 3.0;
    StringView layout = "auto", mode = "2d";
    if (!arg_uint(c, "players", players, 1, 4, false) || !arg_number(c, "spacing", spacing, 0.5, 100) || !arg_string(c, "layout", layout, false) ||
        !arg_string(c, "mode", mode, false))
        return false;
    u32 layout_id = 0;
    static const char* const kLayouts[] = {"auto", "columns", "rows", "grid"};
    for (u32 i = 0; i < 4; ++i)
        if (layout == kLayouts[i]) layout_id = i + 1;
    if (layout_id == 0) return arg_missing(c, "layout", "\"auto\", \"columns\", \"rows\" or \"grid\"");
    --layout_id;
    if (mode != "2d" && mode != "3d") return arg_missing(c, "mode", "\"2d\" (a top-down Camera2D per player) or \"3d\" (a Camera3D per player)");
    asset::Guid prefab;
    if (const cook::JsonValue p = arg(c, "prefab"); p.valid()) {
        StringView ref = p.is_string() ? p.as_string() : StringView();
        if (ref.size() > 5 && ref.substr(0, 5) == "guid:") ref = ref.substr(5);
        const editor::ProjectAsset* a = nullptr;
        if (const Result<asset::Guid> g = asset::parse_guid(ref); g && c.server.session().project().is_open()) a = c.server.session().project().find(g.value());
        if (!a || a->type != asset::ChunkType::Prefab)
            return fail(c, "argument 'prefab' must name a .mcprefab of the project as \"guid:<uuid>\" (asset_list with type Prefab lists them); it needs a LocalPlayer and a LocalTransform");
        prefab = a->guid;
    }
    EditorScene& s = scene(c);
    for (const DocEntity& e : s.doc().entities())
        if (comp<kit::CoopGame>(e)) return fail(c, "the scene already has a CoopGame: change it with component_set, or delete it first");

    Array<EntityId> made;
    const EntityId game_id = s.create_entity(unique_name(s.doc(), "Co-op game").view(), false);
    kit::CoopGame game;
    game.player.guid = prefab;
    game.max_players = static_cast<u32>(players);
    put(s, game_id, game);
    made.push_back(game_id);
    if (players > 1) {
        const EntityId sp = s.create_entity(unique_name(s.doc(), "Split screen").view(), false);
        kit::SplitScreen ss;
        ss.layout = static_cast<kit::SplitLayout>(layout_id);
        ss.divider = 4;
        put(s, sp, ss);
        made.push_back(sp);
    }
    const f64 x0 = -spacing * static_cast<f64>(players - 1) * 0.5;
    for (u32 p = 1; p <= players; ++p) {
        cook::String spawn_name("Spawn");
        cook::appendf(spawn_name, " %u", p);
        const EntityId sp = s.create_entity(unique_name(s.doc(), spawn_name.view()).view(), true);
        LocalTransform lt;
        lt.position = math::vec3{static_cast<f32>(x0 + spacing * (p - 1)), 0.0f, 0.0f};
        s.set(sp, lt);
        kit::PlayerSpawn ps;
        ps.player = p;
        put(s, sp, ps);
        made.push_back(sp);

        cook::String cam_name("Camera P");
        cook::appendf(cam_name, "%u", p);
        const EntityId cam = s.create_entity(unique_name(s.doc(), cam_name.view()).view(), true);
        kit::PlayerCamera pc;
        pc.player = p;
        if (mode == "3d") {
            kit::Camera3DComponent cc;
            cc.primary = p == 1 ? 1 : 0;
            put(s, cam, cc);
        } else {
            kit::Camera2DComponent cc;
            cc.primary = p == 1;
            cc.pixel_snap = false;
            cc.fit_height = 10.0f;
            put(s, cam, cc);
            pc.offset = math::vec3{0.0f, 0.0f, 0.0f};
            pc.smoothing = 8.0f;
        }
        put(s, cam, pc);
        kit::CameraView cv;
        cv.split = p;
        cv.enabled = 0;
        put(s, cam, cv);
        made.push_back(cam);
    }
    c.out.begin_object().key("players").num_u(players).key("entities").begin_array();
    for (const EntityId id : made) c.out.begin_object().key("id").num_u(id).key("name").str(s.doc().find(id)->name.view()).end_object();
    c.out.end_array();
    c.out.key("next").str("add a .mcinput with a map per player (\"player\": 1..4: Move, Join, Leave; samples/09_coop/assets/input/coop.mcinput), "
                          "then player_list to check the rig and render_frame with input \"5 P2/Join\" to see it");
    c.out.end_object();
    return true;
}

const ToolDef kViewTools[] = {
    {"view_list",
     "The open scene's views: every camera with a CameraView (window or into a RenderTexture, viewport, order, split-screen cell, "
     "its PlayerCamera), the SplitScreen layout and each RenderTexture with its cameras and the surfaces that show it, plus "
     "'problems' (a texture nothing draws into, a view into an entity that is no texture, a split camera nothing enables).",
     R"j({"type":"object","properties":{}})j", view_list},
    {"view_add",
     "Add a render texture to the open scene: the RenderTexture entity, the camera that fills it (2D with an optional 'fit' "
     "[width, height] in metres, or 3D looking at 'look_at') and, with 'show_on', a ShowRenderTexture on a Sprite or MeshRef entity "
     "that displays it: a minimap, a mirror, a security monitor, picture-in-picture. Returns the new entity ids.",
     R"j({"type":"object","properties":{"name":{"type":"string","description":"Name of the texture entity (the camera is '<name> camera')"},)j"
     R"j("camera":{"type":"string","enum":["2d","3d"],"default":"2d"},"width":{"type":"integer","minimum":1,"maximum":4096,"default":256},)j"
     R"j("height":{"type":"integer","minimum":1,"maximum":4096,"default":256},"update_hz":{"type":"number","minimum":0,"maximum":240,"description":"Redraws a second; 0 = every frame"},)j"
     R"j("position":{"type":"array","items":{"type":"number"},"description":"Camera position [x, y, z]"},"fit":{"type":"array","items":{"type":"number"},"description":"2D: metres visible [width, height]"},)j"
     R"j("look_at":{"type":"array","items":{"type":"number"},"description":"3D: the point the camera looks at"},)j"
     R"j("show_on":{"description":"Entity (id or name) with a Sprite or MeshRef that shows the texture","type":["integer","string"]}},"required":["name"]})j",
     view_add},
    {"player_list",
     "The open scene's local co-op setup: the CoopGame (prefab, max players, auto join) and for each player 1..4 their PlayerSpawn "
     "entities, PlayerCamera entities and the project's input maps that carry \"player\": n, plus 'problems' (no CoopGame, a player "
     "with no spawn, camera or input map, a camera whose split does not match its player).",
     R"j({"type":"object","properties":{}})j", player_list},
    {"player_setup",
     "Build a local co-op rig in the open scene in one call: a CoopGame (with the 'prefab' every player is spawned from), a "
     "SplitScreen, and for each of 'players' (1-4) a PlayerSpawn and a PlayerCamera with its split-screen CameraView. Add the "
     ".mcinput maps (\"player\": 1..4) yourself; player_list then checks the whole rig. Join and leave are the players' Join and "
     "Leave actions; 'P2/Join' in a run_headless or render_frame input script joins the second player.",
     R"j({"type":"object","properties":{"players":{"type":"integer","minimum":1,"maximum":4,"default":2},)j"
     R"j("prefab":{"type":"string","description":"\"guid:<uuid>\" of the player .mcprefab (needs LocalPlayer and LocalTransform); without one players are coloured squares"},)j"
     R"j("layout":{"type":"string","enum":["auto","columns","rows","grid"],"default":"auto"},)j"
     R"j("mode":{"type":"string","enum":["2d","3d"],"default":"2d","description":"A top-down Camera2D or a Camera3D per player"},)j"
     R"j("spacing":{"type":"number","minimum":0.5,"maximum":100,"default":3,"description":"Metres between the spawns, in a row about the origin"}}})j",
     player_setup},
};

}  // namespace

Span<const ToolDef> views_tool_table() { return Span<const ToolDef>(kViewTools, sizeof(kViewTools) / sizeof(kViewTools[0])); }

}  // namespace mce::mcp
