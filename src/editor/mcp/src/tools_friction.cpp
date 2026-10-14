// report_friction (ROADMAP M-AGENT step 8): an agent that hits something the engine makes hard
// writes it down as data instead of working around it silently. Notes append to the project's
// friction.jsonl, one JSON object per line (several agents append at once without clobbering each
// other); `mce friction <projects...>` collects them into the engine's backlog.

#include <cstdio>
#include <ctime>

#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

namespace {

bool one_of(StringView v, std::initializer_list<const char*> options) {
    for (const char* o : options)
        if (v == o) return true;
    return false;
}

}  // namespace

bool report_friction(ToolCall& c) {
    StringView summary, severity, area, tool, expected, actual, workaround, details;
    if (!need_project(c) || !arg_string(c, "summary", summary, true) || !arg_string(c, "severity", severity, false) ||
        !arg_string(c, "area", area, true) || !arg_string(c, "tool", tool, false) || !arg_string(c, "expected", expected, false) ||
        !arg_string(c, "actual", actual, false) || !arg_string(c, "workaround", workaround, false) ||
        !arg_string(c, "details", details, false))
        return false;
    if (summary.empty() || summary.size() > 200) return arg_missing(c, "summary", "one line, 1-200 characters");
    if (area.empty() || area.size() > 60) return arg_missing(c, "area", "a tool or engine area, e.g. render_frame, physics2d, docs");
    if (severity.empty()) severity = "major";
    if (!one_of(severity, {"blocking", "major", "minor"})) return arg_missing(c, "severity", "blocking, major or minor");
    if (severity == "blocking" && workaround.empty() && actual.empty())
        return arg_missing(c, "actual", "what happened (a blocking note needs actual or workaround)");

    const cook::String file = cook::path_join(c.server.session().project().root(), "friction.jsonl");
    cook::String text;
    (void)cook::fs::read_text(file.view(), text);
    // Count the notes already there, and the ones about the same area (a repeat is a signal too).
    u32 count = 0, same_area = 0;
    for (usize at = 0; at < text.size();) {
        usize end = text.view().find('\n', at);
        if (end == StringView::npos) end = text.size();
        const StringView line = text.view().substr(at, end - at);
        at = end + 1;
        if (line.empty()) continue;
        ++count;
        cook::JsonDoc doc;
        cook::JsonError je;
        if (doc.parse(line, je) && doc.root().get("area").is_string() && doc.root().get("area").as_string() == area) ++same_area;
    }

    cook::String line;
    JsonWriter w(line);
    w.begin_object().key("id").num_u(count + 1).key("time").num_i(static_cast<i64>(std::time(nullptr)));
    w.key("agent").str(c.server.agent()).key("server").str(kServerVersion);
    w.key("severity").str(severity).key("area").str(area).key("summary").str(summary);
    if (!tool.empty()) w.key("tool").str(tool);
    if (!expected.empty()) w.key("expected").str(expected);
    if (!actual.empty()) w.key("actual").str(actual);
    if (!workaround.empty()) w.key("workaround").str(workaround);
    if (!details.empty()) w.key("details").str(details);
    if (!c.server.scene_file().rel.empty()) w.key("scene").str(c.server.scene_file().rel.view());
    w.end_object();
    line += '\n';
    std::FILE* f = std::fopen(file.c_str(), "ab");  // one append of one line: agents do not interleave
    if (!f) return fail(c, "cannot write friction.jsonl in the project folder");
    const bool ok = std::fwrite(line.c_str(), 1, line.size(), f) == line.size();
    std::fclose(f);
    if (!ok) return fail(c, "cannot write friction.jsonl in the project folder");

    c.out.begin_object().key("id").num_u(count + 1).key("file").str("friction.jsonl").key("notes").num_u(count + 1);
    c.out.key("same_area").num_u(same_area).end_object();
    return true;
}

}  // namespace mce::mcp
