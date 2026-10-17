// asset_import (M-IMPORT step 4; gltf / fbx: M-IMPORT3D step 4): the `mce import aseprite|tiled|ldtk|gltf|fbx` converters over MCP. The
// tool runs tools/mce_cli/mce_import.py (installed beside the server binary, or found in a
// source tree above it) as a child process on a file of the project and returns what it wrote
// and what it warned about; the converters are the same ones the command line runs, so an agent
// and a person get the same bytes. The project is rescanned afterwards, so the new assets can be
// used (scene tools, cook_run) at once.

#include <cstdlib>

#include "mce/cook/fs.h"
#include "mce/platform/library.h"
#include "mce/platform/platform.h"
#include "mce/platform/process.h"
#include "tool_args.h"
#include "tools.h"

namespace mce::mcp {

namespace {

constexpr usize kMaxOutputBytes = 8192;

bool plain_word(StringView s) {
    if (s.empty() || s.size() > 64) return false;
    for (char ch : s) {
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return s[0] != '-';
}

StringView parent_of(StringView rel) {
    const usize slash = rel.rfind('/');
    return slash == StringView::npos ? StringView() : rel.substr(0, slash);
}

// mce_import.py: MCE_IMPORT_SCRIPT, the file beside this executable (an SDK), or tools/mce_cli/ of
// a source tree above it (a build directory inside a checkout).
bool find_importer(cook::String& out) {
    if (const char* env = std::getenv("MCE_IMPORT_SCRIPT"); env && *env && platform::file_exists(env)) {
        out = cook::String(env);
        return true;
    }
    const cook::String beside = cook::path_join(platform::base_path(), "mce_import.py");
    if (platform::file_exists(beside.view())) {
        out = beside;
        return true;
    }
    StringView dir(platform::base_path());
    for (u32 up = 0; up < 8 && !dir.empty(); ++up) {
        while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir = dir.substr(0, dir.size() - 1);
        const cook::String cand = cook::path_join(cook::path_join(cook::path_join(dir, "tools"), "mce_cli"), "mce_import.py");
        if (platform::file_exists(cand.view())) {
            out = cand;
            return true;
        }
        const usize fwd = dir.rfind('/'), back = dir.rfind('\\');
        const usize cut = fwd == StringView::npos ? back : (back == StringView::npos || fwd > back ? fwd : back);
        if (cut == StringView::npos) break;
        dir = dir.substr(0, cut);
    }
    return false;
}

// The Python 3 interpreter: MCE_PYTHON, else python3 (python on Windows) on the PATH. The process API
// takes a path, not a name to search for.
bool find_python(cook::String& out) {
    if (const char* env = std::getenv("MCE_PYTHON"); env && *env) {
        out = cook::String(env);
        return true;
    }
#if defined(_WIN32)
    constexpr char kSep = ';';
    constexpr const char* kNames[] = {"python.exe", "python3.exe"};
#else
    constexpr char kSep = ':';
    constexpr const char* kNames[] = {"python3", "python"};
#endif
    const char* path = std::getenv("PATH");
    if (!path) return false;
    const StringView all(path);
    for (const char* name : kNames) {
        for (usize pos = 0; pos <= all.size();) {
            usize end = all.find(kSep, pos);
            if (end == StringView::npos) end = all.size();
            if (end > pos) {
                const cook::String cand = cook::path_join(all.substr(pos, end - pos), name);
                if (platform::file_exists(cand.view())) {
                    out = cand;
                    return true;
                }
            }
            pos = end + 1;
        }
    }
    return false;
}

bool asset_import(ToolCall& c) {
    StringView tool, file, out_dir, name, level;
    if (!need_project(c) || !arg_string(c, "tool", tool, true) || !arg_string(c, "file", file, true) ||
        !arg_string(c, "out_dir", out_dir, false) || !arg_string(c, "name", name, false) || !arg_string(c, "level", level, false))
        return false;
    bool werror = false;
    const cook::JsonValue ts = arg(c, "tile_size");
    if (!arg_bool(c, "werror", werror)) return false;
    if (tool != "aseprite" && tool != "tiled" && tool != "ldtk" && tool != "gltf" && tool != "fbx")
        return fail(c, "argument 'tool' must be \"aseprite\", \"tiled\", \"ldtk\", \"gltf\" or \"fbx\"");
    const bool model_tool = tool == "gltf" || tool == "fbx";
    if (!asset_rel_valid(file)) return fail(c, "argument 'file' must be a path inside the project like \"art/hero.json\" (forward slashes, no '..')");
    if (!name.empty() && !plain_word(name)) return fail(c, "argument 'name' must be letters, digits, '_' and '-'");
    if (!level.empty() && (level[0] == '-' || level.size() > 128)) return fail(c, "argument 'level' is not a level name");
    if (!level.empty() && tool != "ldtk") return fail(c, "argument 'level' applies to tool \"ldtk\" only");
    if (ts.valid() && (!ts.is_number() || ts.as_number() <= 0 || ts.as_number() > 1000)) return fail(c, "argument 'tile_size' must be a number of metres, above 0");
    if (ts.valid() && (tool == "aseprite" || model_tool)) return fail(c, "argument 'tile_size' applies to tools \"tiled\" and \"ldtk\"");
    // A model is referenced by its GUID, which lives in the .mceasset sidecar beside it: the model must be under assets/.
    if (model_tool && file.substr(0, 7) != "assets/") return fail(c, "argument 'file' must be the model inside assets/ for tools \"gltf\" and \"fbx\", like \"assets/models/room.glb\"");

    editor::EditorProject& project = c.server.session().project();
    const cook::String abs_file = cook::path_join(project.root(), file);
    if (!platform::file_exists(abs_file.view())) {
        c.error += "no file '";
        c.error += file;
        c.error += "' in the project";
        return false;
    }
    // Where the assets go: under assets/ (the cook reads nothing else); by default beside the export
    // when that is in assets/, else assets/imported.
    cook::String dir_rel;
    if (!out_dir.empty()) {
        dir_rel = cook::String(out_dir);
    } else if (file.substr(0, 7) == "assets/" && !parent_of(file).empty()) {
        dir_rel = cook::String(parent_of(file));
    } else {
        dir_rel = cook::String("assets/imported");
    }
    if (!asset_rel_valid(dir_rel.view()) || dir_rel.view().substr(0, 7) != "assets/")
        return fail(c, "argument 'out_dir' must be a folder under assets/, like \"assets/levels\"");
    const cook::String abs_dir = cook::path_join(project.root(), dir_rel.view());

    cook::String script;
    if (!find_importer(script))
        return fail(c, "mce_import.py was not found (it is installed beside the server; MCE_IMPORT_SCRIPT can name it)");
    cook::String python;
    if (!find_python(python)) return fail(c, "Python 3 was not found on the PATH (imports need it; MCE_PYTHON can name the interpreter)");

    Array<cook::String> hold;  // keeps the formatted arguments alive
    hold.reserve(16);
    Array<StringView> args;
    args.push_back(python.view());
    args.push_back(script.view());
    args.push_back("--merge-output");  // the process API captures stdout only: warnings go there too
    args.push_back(tool);
    args.push_back(abs_file.view());
    if (tool == "aseprite") {
        // The sheet goes to <out_dir>/<name or file stem>.mcsheet.
        cook::String stem(name);
        if (stem.empty()) {
            StringView leaf = file.substr(parent_of(file).empty() ? 0 : parent_of(file).size() + 1);
            const usize dot = leaf.rfind('.');
            stem = cook::String(dot == StringView::npos ? leaf : leaf.substr(0, dot));
        }
        hold.push_back(cook::path_join(abs_dir.view(), stem.view()));
        hold.back() += ".mcsheet";
        args.push_back("--out");
        args.push_back(hold.back().view());
    } else {
        args.push_back("--out-dir");
        args.push_back(abs_dir.view());
    }
    cook::String cook_exe;
    if (tool == "fbx") {
        // The FBX is read by mce_cook (--dump-fbx-scene): the one beside this server (an SDK), else the script's own search.
        cook_exe = cook::path_join(platform::base_path(), "mce_cook");
#if defined(_WIN32)
        cook_exe += ".exe";
#endif
        if (platform::file_exists(cook_exe.view())) {
            args.push_back("--cook");
            args.push_back(cook_exe.view());
        }
    }
    if (!name.empty()) {
        args.push_back("--name");
        args.push_back(name);
    }
    if (!level.empty()) {
        args.push_back("--level");
        args.push_back(level);
    }
    if (ts.valid()) {
        hold.push_back(cook::String());
        char buf[48];
        std::snprintf(buf, sizeof buf, "%.6g", ts.as_number());
        hold.back() = cook::String(buf);
        args.push_back("--tile-size");
        args.push_back(hold.back().view());
    }
    if (werror) args.push_back("--werror");

    platform::Process proc;
    if (Result<void> r = proc.start(Span<const StringView>(args.data(), args.size()), true); !r) {
        c.error += "cannot start ";
        c.error += python.view();
        c.error += " (Python 3 is needed for imports; MCE_PYTHON names it)";
        return false;
    }
    Array<char> output;
    const i32 code = proc.wait(&output);
    const usize from = output.size() > kMaxOutputBytes ? output.size() - kMaxOutputBytes : 0;
    const StringView text(output.data() + from, output.size() - from);
    if (code != 0) {
        c.error += "the converter failed (nothing was written by it):\n";
        c.error += text;
        return false;
    }
    project.rescan();

    // "wrote <path>" lines are the files; "warning: ..." lines the converter's warnings.
    c.out.begin_object();
    c.out.key("ok").boolean(true).key("tool").str(tool).key("out_dir").str(dir_rel.view());
    c.out.key("output").str(text);
    u32 warnings = 0;
    for (usize pos = 0; pos < text.size();) {
        usize end = text.find('\n', pos);
        if (end == StringView::npos) end = text.size();
        if (text.substr(pos, end - pos).find("warning:") != StringView::npos) ++warnings;
        pos = end + 1;
    }
    c.out.key("warnings").num_u(warnings);
    c.out.end_object();
    return true;
}

const ToolDef kImportTools[] = {
    {"asset_import",
     "Convert an export of a 2D authoring tool into engine assets, with the same converters as `mce import`. "
     "tool \"aseprite\": Aseprite's JSON export (+ its PNG) becomes a .mcsheet (frames as regions, tags as clips). "
     "tool \"tiled\": a .tmj map becomes a .mcscene plus a .mctilemap per tile layer (solid tiles and rectangle "
     "objects become Collider2D boxes). tool \"ldtk\": an .ldtk project becomes a scene per level (IntGrid cells "
     "become boxes, entities become markers). tool \"gltf\" / \"fbx\": a model under assets/ (.gltf, .glb, .fbx) gets a "
     ".mcprefab with a root MeshRef to it and a child per light or camera (PointLight3D, SpotLight3D, Camera3DComponent at the "
     "node's world transform; a directional light is a warning naming Environment3D.sun_direction); the model's sidecar is "
     "created when missing. 'file' is a path inside the project (the export need not be under "
     "assets/); the assets are written under 'out_dir' (default: beside the export when it is in assets/, else "
     "assets/imported). Re-importing keeps GUIDs. Features the engine cannot carry are warnings in 'output'; "
     "werror: true turns them into a failure that writes nothing. Needs Python 3.",
     R"json({"type":"object","properties":{"tool":{"type":"string","enum":["aseprite","tiled","ldtk","gltf","fbx"]},"file":{"type":"string","description":"Export inside the project, e.g. art/hero.json, assets/levels/cave.tmj, assets/levels/world.ldtk; for gltf / fbx the model, e.g. assets/models/room.glb"},"out_dir":{"type":"string","description":"Folder under assets/ to write to, e.g. assets/levels"},"name":{"type":"string","description":"Base name of the written files (default: the export's file name)"},"level":{"type":"string","description":"ldtk only: import just the level with this identifier"},"tile_size":{"type":"number","description":"tiled / ldtk: metres per tile (default 1)"},"werror":{"type":"boolean","default":false}},"required":["tool","file"]})json",
     asset_import},
};

}  // namespace

Span<const ToolDef> import_tool_table() { return Span<const ToolDef>(kImportTools, sizeof(kImportTools) / sizeof(kImportTools[0])); }

}  // namespace mce::mcp
