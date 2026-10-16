// Self-contained knowledge (ROADMAP M-AGENT step 4): the user guide, the engine docs, the worked
// samples and example projects as mce://docs/... resources, docs_search over them, the MCP prompts
// and engine_capabilities. Everything is read from the files the SDK installs (share/doc/mce,
// share/mce/samples, share/mce/examples), or from the source tree in a build tree, so what an agent
// reads is what ships.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "mce/convar.h"
#include "mce/cook/fs.h"
#include "mce/cook/importer.h"
#include "mce/mcp/values.h"
#include "mce/platform/platform.h"
#include "tool_args.h"
#include "tools_agent.h"
#include "tools_docs.h"

namespace mce::mcp {

namespace {

// ---------------------------------------------------------------- where the docs are

struct DocsRoots {
    cook::String docs;      // has user-guide/, engine/, architecture/
    cook::String samples;   // samples/<name>/...
    cook::String examples;  // <Name>/... (the SDK's example projects)
    cook::String library;   // library.json and the library's own files (tools_content.cpp)
    cook::String templates; // the project templates (library entries may name their fonts)
    bool source_tree = false;
};

bool has_guide(StringView docs) {
    cook::String p(docs);
    p += "/user-guide/README.md";
    return cook::fs::exists(p.view());
}

cook::String join(StringView a, StringView b) { return cook::path_join(a, b); }

DocsRoots find_roots(const Server& server) {
    DocsRoots r;
    if (server.options().docs && *server.options().docs) {
        r.docs = cook::fs::absolute(server.options().docs);
        r.samples = join(r.docs.view(), "../../mce/samples");
        r.examples = join(r.docs.view(), "../../mce/examples");
        r.library = join(r.docs.view(), "../../mce/library");
        r.templates = join(r.docs.view(), "../../mce/templates");
        return r;
    }
    // An installed SDK: <prefix>/bin/mce_mcp next to <prefix>/share/doc/mce.
    const cook::String base(platform::base_path());
    for (const char* rel : {"../share/doc/mce", "../../share/doc/mce"}) {
        cook::String d = cook::fs::absolute(join(base.view(), rel).view());
        if (!has_guide(d.view())) continue;
        r.docs = d;
        r.samples = cook::fs::absolute(join(d.view(), "../../mce/samples").view());
        r.examples = cook::fs::absolute(join(d.view(), "../../mce/examples").view());
        r.library = cook::fs::absolute(join(d.view(), "../../mce/library").view());
        r.templates = cook::fs::absolute(join(d.view(), "../../mce/templates").view());
        return r;
    }
#if defined(MCE_MCP_SOURCE_ROOT)
    // A build tree: the sources it was configured from.
    r.docs = join(MCE_MCP_SOURCE_ROOT, "docs");
    r.samples = join(MCE_MCP_SOURCE_ROOT, "samples");
    r.examples = join(MCE_MCP_SOURCE_ROOT, "projects");
    r.library = join(MCE_MCP_SOURCE_ROOT, "library");
    r.templates = join(MCE_MCP_SOURCE_ROOT, "templates");
    r.source_tree = true;
#endif
    return r;
}

// Cached per server and --docs value (a later server can live at the same address as an earlier
// one, as consecutive test cases' servers do in a release build).
const DocsRoots& roots(const Server& server) {
    static DocsRoots r;
    static const Server* for_server = nullptr;
    static cook::String for_docs;
    const StringView docs = server.options().docs ? StringView(server.options().docs) : StringView();
    if (for_server != &server || for_docs.view() != docs) {
        r = find_roots(server);
        for_server = &server;
        for_docs = cook::String(docs);
    }
    return r;
}

}  // namespace

cook::String content_root(const Server& server, StringView group) {
    const DocsRoots& r = roots(server);
    if (group == "docs") return r.docs;
    if (group == "samples") return r.samples;
    if (group == "examples") return r.examples;
    if (group == "library") return r.library;
    if (group == "templates") return r.templates;
    return {};
}

namespace {

// The example projects the SDK ships (cmake/MceInstall.cmake); in a source tree projects/ holds more.
constexpr const char* kExamples[] = {"Kindle", "Undelivered", "Room3D"};

// URI groups: mce://docs/<group>/<path> -> <root>/<sub>/<path>.
struct Group {
    const char* name;
    const char* description;
};
constexpr Group kGroups[] = {
    {"guide", "The user guide: how to make a game with the engine, chapter by chapter"},
    {"engine", "Engine reference: one page per module"},
    {"architecture", "Architecture notes"},
    {"samples", "Worked samples: small complete programs, one per feature"},
    {"examples", "Example game projects (Kindle: 2D platformer, Undelivered: visual novel, Room3D: 3D kit)"},
};

bool safe_rel(StringView p) {
    if (p.empty()) return true;
    if (p[0] == '/' || p[0] == '\\' || p.find(':') != StringView::npos || p.find('\\') != StringView::npos) return false;
    usize at = 0;
    while (at <= p.size()) {
        usize end = p.find('/', at);
        if (end == StringView::npos) end = p.size();
        if (p.substr(at, end - at) == "..") return false;
        at = end + 1;
    }
    return true;
}

bool ends_with(StringView s, StringView suffix) { return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix; }

bool text_file(StringView name) {
    for (const char* ext : {".md", ".cpp", ".h", ".txt", ".mcscene", ".mcprefab", ".mcseq", ".mcpart", ".mctilemap", ".mctest",
                            ".mcinput", ".mcmat", ".mcconfig", ".mceproj", ".json", ".cmake", ".py"})
        if (ends_with(name, ext)) return true;
    return ends_with(name, "CMakeLists.txt");
}

bool skipped_dir(StringView rel) {
    for (const char* d : {"build/", "cooked/", "dist/", "__pycache__/"})
        if (rel.substr(0, std::strlen(d)) == d || rel.find(cook::String("/").append(d).view()) != StringView::npos) return true;
    return false;
}

// The file system directory of a group; false for an unknown group.
bool group_dir(const DocsRoots& r, StringView group, cook::String& out) {
    if (group == "guide") out = join(r.docs.view(), "user-guide");
    else if (group == "engine") out = join(r.docs.view(), "engine");
    else if (group == "architecture") out = join(r.docs.view(), "architecture");
    else if (group == "samples") out = r.samples;
    else if (group == "examples") out = r.examples;
    else return false;
    return true;
}

bool example_allowed(StringView rel) {
    for (const char* e : kExamples) {
        const usize n = std::strlen(e);
        if (rel.substr(0, n) == e && (rel.size() == n || rel[n] == '/')) return true;
    }
    return false;
}

// Every served file of a group, relative to its directory, sorted.
void group_files(const DocsRoots& r, StringView group, Array<cook::String>& out) {
    cook::String dir;
    if (!group_dir(r, group, dir) || !cook::fs::is_directory(dir.view())) return;
    Array<cook::String> all;
    if (!cook::fs::list_files(dir.view(), all)) return;
    const bool docs_group = group == "guide" || group == "engine" || group == "architecture";
    for (cook::String& f : all) {
        if (docs_group ? !ends_with(f.view(), ".md") : !text_file(f.view())) continue;
        if (skipped_dir(f.view())) continue;
        if (group == "examples" && !example_allowed(f.view())) continue;
        out.push_back(static_cast<cook::String&&>(f));
    }
}

// The first "# " heading of a markdown file, for resource names.
cook::String title_of(StringView text, StringView fallback) {
    if (text.substr(0, 2) == "# ") {
        const usize nl = text.find('\n');
        return cook::String(text.substr(2, (nl == StringView::npos ? text.size() : nl) - 2));
    }
    return cook::String(fallback);
}

// ---------------------------------------------------------------- search index

struct Chunk {
    cook::String uri;
    cook::String heading;
    u32 line = 1;
    cook::String text;  // lowercase copy for matching
    cook::String raw;   // as written, for snippets
};

struct Index {
    cook::String key;
    Array<Chunk> chunks;
};

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool word_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (c >= 'A' && c <= 'Z'); }

cook::String lowered(StringView s) {
    cook::String out;
    for (const char c : s) out += lower(c);
    return out;
}

void add_chunk(Index& idx, StringView uri, StringView heading, u32 line, StringView text) {
    if (text.empty()) return;
    Chunk ch;
    ch.uri = cook::String(uri);
    ch.heading = cook::String(heading);
    ch.line = line;
    ch.raw = cook::String(text);
    ch.text = lowered(text);
    idx.chunks.push_back(static_cast<Chunk&&>(ch));
}

// Markdown: one chunk per heading section. Code and data: one per 60 lines.
void index_file(Index& idx, StringView uri, StringView name, StringView text, bool markdown) {
    usize start = 0;
    u32 line = 1, start_line = 1, lines_in = 0;
    cook::String heading(name);
    usize pos = 0;
    while (pos <= text.size()) {
        usize end = text.find('\n', pos);
        if (end == StringView::npos) end = text.size();
        const StringView l = text.substr(pos, end - pos);
        const bool new_section = markdown ? (l.size() > 1 && l[0] == '#' && pos != 0) : lines_in == 60;
        if (new_section) {
            add_chunk(idx, uri, heading.view(), start_line, text.substr(start, pos - start));
            start = pos;
            start_line = line;
            lines_in = 0;
        }
        if (markdown && l.size() > 1 && l[0] == '#') {
            usize h = 0;
            while (h < l.size() && (l[h] == '#' || l[h] == ' ')) ++h;
            heading = cook::String(l.substr(h));
        }
        ++lines_in;
        ++line;
        pos = end + 1;
    }
    add_chunk(idx, uri, heading.view(), start_line, text.substr(start));
}

const Index& search_index(const DocsRoots& r) {
    static Index idx;
    if (idx.key == r.docs && !idx.chunks.empty()) return idx;
    idx = Index{};
    idx.key = r.docs;
    for (const Group& g : kGroups) {
        Array<cook::String> files;
        group_files(r, g.name, files);
        cook::String dir;
        group_dir(r, g.name, dir);
        for (const cook::String& f : files) {
            const bool md = ends_with(f.view(), ".md");
            if (!md && !ends_with(f.view(), ".cpp") && !ends_with(f.view(), ".h")) continue;  // search prose and code
            cook::String text;
            if (!cook::fs::read_text(join(dir.view(), f.view()).view(), text)) continue;
            cook::String uri("mce://docs/");
            uri += g.name;
            uri += '/';
            uri += f.view();
            index_file(idx, uri.view(), f.view(), text.view(), md);
        }
    }
    return idx;
}

usize count_of(StringView hay, StringView needle) {
    usize n = 0;
    for (usize at = hay.find(needle); at != StringView::npos; at = hay.find(needle, at + needle.size())) {
        const bool left = at == 0 || !word_char(hay[at - 1]);
        if (left) ++n;
    }
    return n;
}

void snippet(const Chunk& ch, Span<const cook::String> terms, cook::String& out) {
    usize at = StringView::npos;
    for (const cook::String& t : terms) {
        const usize p = ch.text.view().find(t.view());
        if (p < at) at = p;
    }
    if (at == StringView::npos) at = 0;
    // Both ends on UTF-8 character boundaries: a cut inside a multi-byte character is not valid
    // JSON text for the client.
    const StringView raw = ch.raw.view();
    const auto continuation = [&](usize i) { return i < raw.size() && (static_cast<u8>(raw[i]) & 0xC0) == 0x80; };
    usize from = at > 120 ? at - 120 : 0;
    while (from > 0 && continuation(from)) --from;
    usize len = std::min<usize>(raw.size() - from, 320);
    while (len > 0 && continuation(from + len)) --len;
    bool space = false;
    if (from) out += "...";
    for (const char c : ch.raw.view().substr(from, len)) {
        const bool ws = c == '\n' || c == '\r' || c == '\t' || c == ' ';
        if (ws) {
            if (!space) out += ' ';
            space = true;
        } else {
            out += c;
            space = false;
        }
    }
    if (from + len < ch.raw.size()) out += "...";
}

// ---------------------------------------------------------------- prompts

struct PromptArg {
    const char* name;
    const char* description;
    bool required;
};

struct Prompt {
    const char* name;
    const char* description;
    PromptArg args[2];
    usize arg_count;
    const char* text;  // {brief}, {name}, {focus} are replaced
};

const Prompt kPrompts[] = {
    {"start_game",
     "Build a game from a one-paragraph brief, start to finish, with no human in the loop",
     {{"brief", "What the game is: genre, look, the one rule that makes it fun", true}, {}},
     1,
     "Make this game with Mesh Control Engine through this MCP server, with no human in the loop:\n\n{brief}\n\n"
     "Work in this order and check each step before the next:\n"
     "1. Read mce://docs/guide/14-mcp-ai-workflow.md (the tools and the workflow) and mce://docs/guide/README.md. "
     "Call engine_capabilities to see what is data (components, assets, MCP tools) and what needs game code.\n"
     "2. Find the closest worked sample or example with docs_search (\"platformer\", \"visual novel\", \"3d kit\") and read "
     "its scene and code under mce://docs/samples/ or mce://docs/examples/.\n"
     "3. Components and systems the game needs that the engine lacks go in the game module: edit it, then module_build "
     "(errors come back as file:line:col) and module_test. A 2D game of existing components plus rules that fit in a few "
     "functions needs no module: write Luau scripts instead (mce://docs/guide/27-scripting.md; script_check checks one "
     "without a build; ScriptVars is a script's memory) and keep the project a script project (empty \"games\" list).\n"
     "4. Author the scenes with entity_create / component_set, save with scene_save, verify with run_headless (inspect, "
     "trace) and look with render_frame after every visible change. Give the game its own look: a new project has "
     "starter themes in assets/ui/themes (pixel, clean, story); name the one that fits the brief as each .mcui "
     "document's theme with ui_theme_set (or a UiScreen's theme), or copy one and change its palette and fonts.\n"
     "   A minimap, mirror, monitor or split-screen co-op game: view_add and player_setup build the components, view_list and "
     "player_list name what is missing, render_frame (view, input \"5 P2/Join\") looks at one player or one texture.\n"
     "5. Write .mctest scenarios under tests/ (a walk_to bot that must finish each level, no log errors) and run them "
     "with test_run; use playtest's feel report (stuck spots, deaths, idle time) to fix levels.\n"
     "6. Keep goldens of the title and each level with image_diff, so later changes that break a screen show up.\n"
     "7. Finish: a title, win and lose, credits for every asset (CREDITS.md; gen_* and library_add write the rows), "
     "project_check until pass is true, then ship_build (mce://docs/guide/15-shipping.md has the rest).\n"
     "Prefer data over code, name every entity, save often, and never edit cooked/ by hand. Working with other agents: "
     "task_claim your work, asset_lock what you rework, project_watch what they did. When the engine makes something "
     "hard, call report_friction before you work around it."},
    {"add_level",
     "Add one level to the open project: author it, prove it can be finished, and keep a test for it",
     {{"name", "The level's name (its scene file is scenes/<name>.mcscene)", true},
      {"focus", "What the level is about: a new mechanic, a difficulty step, a mood", false}},
     2,
     "Add the level \"{name}\" to this project. {focus}\n\n"
     "1. scene_info and scene_list_entities on an existing level to reuse its structure (camera, player, goal, HUD); "
     "asset_list for the art you can use.\n"
     "2. scene_new {\"name\": \"{name}\"} (or scene_open + scene_save to a new path to start from a copy), author it with "
     "entity_create / entity_duplicate / component_set, and scene_save to scenes/{name}.mcscene.\n"
     "3. render_sheet over a few ticks to see it; fix what reads badly.\n"
     "4. playtest with a walk_to bot from the player to the goal; if it does not finish or reports stuck spots, change "
     "the level until it does.\n"
     "5. Write tests/{name}.mctest (the bot finishes, deaths within a budget, no log errors) and run test_run.\n"
     "6. Hook the level into the game's level order (the project's .mceproj or the game's own list) and save a golden "
     "of its first frame with image_diff."},
    {"polish_pass",
     "A polish pass over the open project: look at every screen, playtest, and fix what feels rough",
     {{"focus", "Optional: what to look at first (feel, readability, audio, performance)", false}, {}},
     1,
     "Do a polish pass on this project. {focus}\n\n"
     "1. test_run: every scenario must pass before and after the pass.\n"
     "2. For each scene: render_sheet at a few ticks (and with safe_area {\"top\": 48, \"bottom\": 34} for phones); note "
     "unreadable text, empty space, art that does not match, UI outside the safe area.\n"
     "3. playtest each level with walk_to and explore bots: long idle_s, stuck spots and deaths in odd places are the "
     "rough edges; render_frame there with the bot's input to see why.\n"
     "4. Fix by data first (component values, particles, sounds, camera follow and shake), code only when needed "
     "(module_build).\n"
     "5. run_headless metrics and the log: no warnings or errors left.\n"
     "6. Update the goldens you changed on purpose (image_diff update: true) and run test_run again."},
};

void replace_all(cook::String& s, StringView from, StringView to) {
    cook::String out;
    usize at = 0;
    const StringView v = s.view();
    for (usize p = v.find(from); p != StringView::npos; p = v.find(from, at)) {
        out += v.substr(at, p - at);
        out += to;
        at = p + from.size();
    }
    out += v.substr(at);
    s = out;
}

// ---------------------------------------------------------------- capabilities

// Known features and how a game reaches them today, kept by hand (the M-DATA-REACH table; that
// track turns "code" rows into data). "data" means a component or an asset an agent can author.
struct Feature {
    const char* area;
    const char* feature;
    const char* reach;  // data | asset | mcp | console | code
    const char* how;
};
constexpr Feature kFeatures[] = {
    {"2D", "sprites, text, tilemaps, parallax, 2D lights, post effects", "data", "Sprite, TextLabel, TilemapLayer (.mctilemap), Parallax, Light2D, Post2DComponent"},
    {"2D", "particles", "data", "ParticleEmitter + .mcpart"},
    {"2D", "sprite animation and atlases", "data", "AnimatedSprite + .mcsheet (gen_pixels or gen_sprite frames side by side)"},
    {"2D", "camera follow and shake", "data", "Camera2DComponent + CameraFollow2D, CameraShake2D"},
    {"physics", "2D bodies, colliders, triggers", "data", "RigidBody2D, Collider2D"},
    {"physics", "3D static colliders", "data", "the 3D kit's colliders"},
    {"physics", "3D dynamic bodies, character controller", "data", "RigidBody3D, CharacterController3D (and CameraFollow3D)"},
    {"gameplay", "pathfinding", "data", "PathAgent + a kit::NavGrid resource (the grid is code)"},
    {"gameplay", "sequences and state machines", "data", "SequencePlayer + .mcseq; kit FSM"},
    {"gameplay", "game rules", "code", "the game module's systems (module_build reloads them)"},
    {"audio", "music and ambience", "data", "AudioSource"},
    {"audio", "one-shot sound effects", "data", "SfxTrigger"},
    {"audio", "compressed clips (Ogg Vorbis)", "asset", ".ogg, or \"compress\": \"vorbis\" in a .wav sidecar"},
    {"audio", "varied one-shots (sound sets, pitch)", "asset", ".mcsound named as the clip; PlayParams::pitch from code"},
    {"audio", "bus filters, reverb, limiter", "data", "AudioBusEffects, AudioReverb, AudioLimiter"},
    {"audio", "ducking, mixer snapshots, a pause mix", "data", "AudioDuck, AudioSnapshot (while_paused)"},
    {"audio", "occlusion through walls, Doppler", "data", "AudioOcclusion, AudioDoppler next to a 3D AudioSource"},
    {"audio", "an offline render of a run", "data", "mce_player -headless 1 -audio_record run.wav"},
    {"input", "actions, bindings, touch controls", "asset", ".mcinput"},
    {"ui", "HUD and menus, themes, a screen's navigation", "data",
     "UiScreen + a .mcui document (ui_create, ui_widget_add/set/remove, ui_theme_set, ui_preview); UiFlow shows, hides, pauses, "
     "loads a scene and quits on input actions; mce::ui from code only for what a document cannot say"},
    {"3D", "meshes, materials, lights, sky", "data", "the 3D kit's components, glTF, .mcmat, .mcenv"},
    {"store and ads", "purchases, ads", "asset", ".mcstore, .mcservices"},
    {"live services", "remote config, crash reports", "asset", ".mcconfig, .mcservices"},
    {"scenes", "prefabs", "data", "instances in .mcscene (entity_create with a prefab); prefab_create_from, prefab_apply, prefab_revert"},
    {"scenes", "scene changes (title, levels, win and lose)", "data",
     "UiFlow load_on on a UiScreen (a Play, Retry or Next button loads its scene); from game rules: scene::request_change(world, \"scenes/level2.mcscene\") "
     "or mce.load_scene(path) in Luau; a .mctest checks {\"scene\": ...}"},
    {"scenes", "scenes, UI documents, timelines, 2D skeletons, behaviour trees, terrain", "mcp",
     "entity_*, component_*, scene_*, ui_*, timeline_*, skel2d_*, bt_*, terrain_*, tilemap_paint, asset_create / asset_write"},
    {"scenes", "streaming", "data", "SceneChunk, StreamingFocus"},
    {"agent", "see a frame, compare to a golden", "mcp", "render_frame, render_sheet, image_diff"},
    {"agent", "rebuild and reload game code", "mcp", "module_build, module_test"},
    {"agent", "playtest without a person", "mcp", "test_run (.mctest), playtest bots, run_headless trace"},
    {"agent", "read the docs and samples", "mcp", "mce://docs/... resources, docs_search, prompts"},
    {"agent", "make art, sound and music without a person", "mcp", "gen_palette, gen_sprite, gen_pixels, gen_tileset, gen_texture, gen_mesh, gen_sfx, gen_music"},
    {"agent", "use CC0/OFL starter content with credits", "mcp", "library_search, library_add"},
    {"agent", "check the project and make the release zip", "mcp", "project_check, ship_build"},
    {"agent", "work with other agents on one project: who did what, locks, shared tasks, merged scene saves", "mcp",
     "project_watch, asset_lock, task_claim, scene_save resolve, scene_switch"},
    {"agent", "report what the engine made hard, for its backlog", "mcp", "report_friction (friction.jsonl; mce friction collects)"},
    {"debug", "console variables and commands", "console", "console_list, console_exec"},
};

}  // namespace

// ---------------------------------------------------------------- resources

void docs_list_resources(Server& server, JsonWriter& w) {
    const DocsRoots& r = roots(server);
    w.begin_object().key("uri").str("mce://docs/index").key("name").str("docs index");
    w.key("description").str("Every doc, sample and example this server serves, by group (start here)").key("mimeType").str("text/markdown").end_object();
    for (const char* g : {"guide", "engine", "architecture"}) {
        Array<cook::String> files;
        group_files(r, g, files);
        cook::String dir;
        group_dir(r, g, dir);
        for (const cook::String& f : files) {
            cook::String text, uri("mce://docs/");
            cook::fs::read_text(join(dir.view(), f.view()).view(), text);
            uri += g;
            uri += '/';
            uri += f.view();
            const cook::String title = title_of(text.view(), f.view());
            w.begin_object().key("uri").str(uri.view()).key("name").str(title.view()).key("mimeType").str("text/markdown").end_object();
        }
    }
    for (const char* g : {"samples", "examples"}) {
        cook::String uri("mce://docs/");
        uri += g;
        uri += '/';
        for (const Group& grp : kGroups)
            if (StringView(grp.name) == g)
                w.begin_object().key("uri").str(uri.view()).key("name").str(g).key("description").str(grp.description).key("mimeType").str("text/markdown").end_object();
    }
}

void docs_list_templates(JsonWriter& w) {
    w.begin_object().key("uriTemplate").str("mce://docs/{group}/{path}").key("name").str("docs");
    w.key("description").str("A doc, sample or example file (group: guide, engine, architecture, samples, examples); a folder path lists its files").end_object();
    w.begin_object().key("uriTemplate").str("mce://docs/components/{name}").key("name").str("component");
    w.key("description").str("One component's fields, kinds, ranges, enum values and help, as refl_describe_component returns them").key("mimeType").str("application/json").end_object();
}

bool docs_read_resource(Server& server, StringView uri, cook::String& text, const char*& mime) {
    constexpr StringView kPrefix = "mce://docs/";
    if (uri.substr(0, kPrefix.size()) != kPrefix) return false;
    const DocsRoots& r = roots(server);
    const StringView rest = uri.substr(kPrefix.size());
    if (rest == "index") {
        mime = "text/markdown";
        text += "# Mesh Control Engine docs\n\nRead with resources/read; search with the docs_search tool.\n";
        for (const Group& g : kGroups) {
            text += "\n## mce://docs/";
            text += g.name;
            text += "/\n\n";
            text += g.description;
            text += "\n\n";
            Array<cook::String> files;
            group_files(r, g.name, files);
            usize shown = 0;
            for (const cook::String& f : files) {
                if ((StringView(g.name) == "samples" || StringView(g.name) == "examples") && !ends_with(f.view(), ".md") &&
                    !ends_with(f.view(), "main.cpp"))
                    continue;  // the folders list the rest
                text += "- mce://docs/";
                text += g.name;
                text += '/';
                text += f.view();
                text += '\n';
                ++shown;
            }
            if (!shown) text += "(none shipped here)\n";
        }
        text += "\n## mce://docs/components/{name}\n\nEvery reflected component (refl_list_components names them).\n";
        return true;
    }
    if (rest.substr(0, 11) == "components/") {
        const refl::TypeRecord* t = refl::find_type(StringId(rest.substr(11)));
        if (!t || !t->is_component()) return false;
        JsonWriter w(text);
        describe_type(w, *t);
        return true;
    }
    const usize slash = rest.find('/');
    const StringView group = slash == StringView::npos ? rest : rest.substr(0, slash);
    StringView path = slash == StringView::npos ? StringView() : rest.substr(slash + 1);
    cook::String dir;
    if (!group_dir(r, group, dir) || !safe_rel(path)) return false;
    if (group == "examples" && !path.empty() && !example_allowed(path)) return false;
    while (!path.empty() && path[path.size() - 1] == '/') path = path.substr(0, path.size() - 1);
    const cook::String abs = path.empty() ? dir : join(dir.view(), path);
    if (cook::fs::is_directory(abs.view())) {
        // A folder: its served files.
        mime = "text/markdown";
        Array<cook::String> files;
        group_files(r, group, files);
        text += "# mce://docs/";
        text += rest;
        text += "\n\n";
        usize n = 0;
        for (const cook::String& f : files) {
            if (!path.empty() && (f.view().substr(0, path.size()) != path || f.size() <= path.size() || f.view()[path.size()] != '/')) continue;
            text += "- mce://docs/";
            text += group;
            text += '/';
            text += f.view();
            text += '\n';
            ++n;
        }
        if (!n) text += "(empty)\n";
        return true;
    }
    if (!text_file(path) || (group != "samples" && group != "examples" && !ends_with(path, ".md"))) return false;
    if (!cook::fs::read_text(abs.view(), text)) return false;
    mime = ends_with(path, ".md") ? "text/markdown" : "text/plain";
    return true;
}

// ---------------------------------------------------------------- prompts

void prompts_list(JsonWriter& w) {
    w.begin_object().key("prompts").begin_array();
    for (const Prompt& p : kPrompts) {
        w.begin_object().key("name").str(p.name).key("description").str(p.description).key("arguments").begin_array();
        for (usize i = 0; i < p.arg_count; ++i)
            w.begin_object().key("name").str(p.args[i].name).key("description").str(p.args[i].description).key("required").boolean(p.args[i].required).end_object();
        w.end_array().end_object();
    }
    w.end_array().end_object();
}

bool prompts_get(StringView name, cook::JsonValue args, JsonWriter& w, cook::String& err) {
    const Prompt* p = nullptr;
    for (const Prompt& q : kPrompts)
        if (name == q.name) p = &q;
    if (!p) {
        err += "unknown prompt: ";
        err += name;
        return false;
    }
    cook::String text(p->text);
    for (usize i = 0; i < p->arg_count; ++i) {
        const cook::JsonValue v = args.valid() ? args.get(p->args[i].name) : cook::JsonValue();
        if (p->args[i].required && (!v.is_string() || v.as_string().empty())) {
            err += "missing argument: ";
            err += p->args[i].name;
            return false;
        }
        cook::String key("{");
        key += p->args[i].name;
        key += '}';
        replace_all(text, key.view(), v.is_string() ? v.as_string() : StringView());
    }
    w.begin_object().key("description").str(p->description).key("messages").begin_array();
    w.begin_object().key("role").str("user").key("content").begin_object().key("type").str("text").key("text").str(text.view()).end_object().end_object();
    w.end_array().end_object();
    return true;
}

// ---------------------------------------------------------------- tools

bool docs_search(ToolCall& c) {
    StringView query;
    u64 limit = 8;
    if (!arg_string(c, "query", query, true) || !arg_uint(c, "limit", limit, 1, 20, false)) return false;
    StringView only;
    if (!arg_string(c, "group", only, false)) return false;
    if (!only.empty()) {
        bool known = false;
        for (const Group& g : kGroups) known = known || only == g.name;
        if (!known) return fail(c, "argument 'group': guide, engine, architecture, samples or examples");
    }
    // Query terms: words of 2+ characters, lowercase.
    Array<cook::String> terms;
    cook::String cur;
    const cook::String q = lowered(query);
    for (usize i = 0; i <= q.size(); ++i) {
        const char ch = i < q.size() ? q.view()[i] : ' ';
        if (word_char(ch)) {
            cur += ch;
            continue;
        }
        if (cur.size() >= 2) terms.push_back(cur);
        cur = cook::String();
    }
    if (terms.empty()) return fail(c, "argument 'query': give words to look for, e.g. \"camera shake\"");
    const DocsRoots& r = roots(c.server);
    const Index& idx = search_index(r);
    if (idx.chunks.empty()) return fail(c, "no docs found: run the server from an SDK install or a build tree, or pass --docs <share/doc/mce>");

    // BM25-like: saturating term frequency weighted by rarity, heading hits count double, all terms
    // together score a bonus.
    Array<f64> idf;
    for (const cook::String& t : terms) {
        usize df = 0;
        for (const Chunk& ch : idx.chunks) df += ch.text.view().find(t.view()) != StringView::npos ? 1 : 0;
        idf.push_back(std::log(1.0 + (static_cast<f64>(idx.chunks.size()) - static_cast<f64>(df) + 0.5) / (static_cast<f64>(df) + 0.5)));
    }
    struct Hit {
        usize chunk;
        f64 score;
    };
    Array<Hit> hits;
    for (usize i = 0; i < idx.chunks.size(); ++i) {
        const Chunk& ch = idx.chunks[i];
        if (!only.empty()) {
            cook::String prefix("mce://docs/");
            prefix += only;
            prefix += '/';
            if (ch.uri.view().substr(0, prefix.size()) != prefix.view()) continue;
        }
        f64 score = 0;
        usize matched = 0;
        const cook::String heading = lowered(ch.heading.view());
        for (usize t = 0; t < terms.size(); ++t) {
            const f64 tf = static_cast<f64>(count_of(ch.text.view(), terms[t].view()));
            if (tf == 0) continue;
            ++matched;
            score += idf[t] * tf * 2.2 / (tf + 1.2);
            if (heading.view().find(terms[t].view()) != StringView::npos) score += idf[t] * 2.0;
        }
        if (!matched) continue;
        if (matched == terms.size() && terms.size() > 1) score *= 1.5;
        if (ends_with(ch.uri.view(), ".md")) score *= 1.2;  // prose explains; code shows
        hits.push_back(Hit{i, score});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score || (a.score == b.score && a.chunk < b.chunk); });
    c.out.begin_object();
    c.out.key("query").str(query);
    c.out.key("matches").num_u(hits.size());
    c.out.key("results").begin_array();
    for (usize i = 0; i < hits.size() && i < limit; ++i) {
        const Chunk& ch = idx.chunks[hits[i].chunk];
        cook::String snip;
        snippet(ch, terms.span(), snip);
        c.out.begin_object().key("uri").str(ch.uri.view()).key("section").str(ch.heading.view()).key("line").num_u(ch.line);
        c.out.key("score").num_f(std::round(hits[i].score * 100.0) / 100.0).key("snippet").str(snip.view()).end_object();
    }
    c.out.end_array();
    c.out.end_object();
    return true;
}

bool engine_capabilities(ToolCall& c) {
    StringView area;
    if (!arg_string(c, "area", area, false)) return false;
    c.out.begin_object();
    c.out.key("features").begin_array();
    for (const Feature& f : kFeatures) {
        if (!area.empty() && area != f.area) continue;
        c.out.begin_object().key("area").str(f.area).key("feature").str(f.feature).key("reach").str(f.reach).key("how").str(f.how).end_object();
    }
    c.out.end_array();
    if (area.empty()) {
        c.out.key("components").begin_array();
        for (const refl::TypeRecord* t : refl::all())
            if (t->is_component()) c.out.str(t->name);
        c.out.end_array();
        // Source files the cook imports, by extension.
        cook::ImporterRegistry reg;
        cook::ImporterRegistry::add_builtins(reg);
        c.out.key("asset_types").begin_array();
        for (const char* ext : {"png", "jpg", "ttf", "otf", "wav", "ogg", "mcsound", "gltf", "glb", "mcscene", "mcprefab", "mcmat", "mcenv", "mcnav", "mcpart",
                                "mcseq", "mctilemap", "mcinput", "mcstore", "mcservices", "mcconfig", "mcui", "csv", "glsl"}) {
            cook::String dotted(".");
            dotted += ext;
            const cook::IImporter* imp = reg.for_extension(dotted.view());
            if (!imp) continue;
            c.out.begin_object().key("extension").str(ext).key("importer").str(imp->id()).end_object();
        }
        c.out.end_array();
        c.out.key("tools").begin_array();
        for (const ToolDef& t : tool_table()) c.out.str(t.name);
        c.out.end_array();
        Array<ConVar*> vars;
        ConVar::collect(vars);
        c.out.key("convars").num_u(vars.size());
        c.out.key("docs").str("mce://docs/index");
    }
    c.out.end_object();
    return true;
}

}  // namespace mce::mcp
