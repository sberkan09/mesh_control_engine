// script_check (M-SCRIPT step 4): the cook's checks on one Luau script without cooking: syntax,
// hidden state, names (with "did you mean") and Luau's type check against mce.d.luau. Every problem
// comes back with its line and column. With 'definitions' the generated mce.d.luau text comes too.
// Where the build has no scripting the tool is not in the table.

#include "mce/cook/fs.h"
#include "tool_args.h"
#include "tools.h"
#if MCE_HAS_SCRIPT
#include "mce/script/compiler.h"
#endif

namespace mce::mcp {

#if MCE_HAS_SCRIPT

namespace {

constexpr u64 kMaxSource = 1u << 20;

bool script_check(ToolCall& c) {
    StringView path;
    StringView source;
    bool with_defs = false;
    if (!arg_string(c, "path", path, false) || !arg_string(c, "source", source, false) || !arg_bool(c, "definitions", with_defs))
        return false;
    if (path.empty() == source.empty()) return fail(c, "pass either 'path' (a .luau file under assets/) or 'source' (the script text)");
    cook::String text;
    cook::String name("source.luau");
    if (!path.empty()) {
        if (!need_project(c)) return false;
        if (path.size() > 7 && path.substr(0, 7) == "assets/") path = path.substr(7);  // as the asset_* tools print it
        if (path.size() < 5 || path.substr(path.size() - 5) != ".luau") return fail(c, "argument 'path': not a .luau file");
        if (!cook::fs::read_text(c.server.session().project().asset_path(path).view(), text))
            return fail(c, "argument 'path': no such file under assets/ (give it relative to assets/, e.g. scripts/player.luau)");
        name = cook::String(path);
        source = text.view();
    }
    if (source.size() > kMaxSource) return fail(c, "the script is larger than 1 MiB");
    Array<script::CompileError> problems;
    script::check(name.view(), source, problems);
    c.out.begin_object().key("file").str(name.view()).key("ok").boolean(problems.empty()).key("problems").begin_array();
    for (const script::CompileError& p : problems)
        c.out.begin_object().key("line").num_u(p.line).key("col").num_u(p.col).key("message").str(p.message.view()).end_object();
    c.out.end_array();
    if (with_defs) {
        Array<char> defs;
        script::write_definitions(defs);
        c.out.key("definitions").str(StringView(defs.data(), defs.size()));
    }
    c.out.end_object();
    return true;
}

const ToolDef kScriptTools[] = {
    {"script_check",
     "Check a Luau script the way the cook does, without cooking: syntax, state kept outside components, "
     "component/event/field names (with a 'did you mean'), removed globals and Luau's type check against the "
     "engine's mce.d.luau (a '--!strict' first line asks for strict typing). Returns every problem with line and "
     "col. 'definitions': also return mce.d.luau (the cook writes it to cooked/mce.d.luau for editors).",
     R"({"type":"object","properties":{"path":{"type":"string","description":"A .luau file under assets/, e.g. scripts/player.luau"},"source":{"type":"string","description":"Script text to check instead of a file"},"definitions":{"type":"boolean","default":false}}})",
     script_check},
};

}  // namespace

Span<const ToolDef> script_tool_table() { return Span<const ToolDef>(kScriptTools, sizeof(kScriptTools) / sizeof(kScriptTools[0])); }

#else

Span<const ToolDef> script_tool_table() { return {}; }

#endif

}  // namespace mce::mcp
