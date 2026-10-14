// M-AGENT closed code loop (SBA-49): module_build compiles the game module with the project's own
// CMake build, returns the compiler's diagnostics as file:line:col JSON and maps the new build into
// the running server (new components and systems with no restart); module_test runs the project's
// ctest tests. The build tree is the one the loaded module was built in: the nearest folder above
// the module that holds a CMakeCache.txt (a checkout's build/<preset>/, an SDK project's build/).

#include <cstdlib>

#include "mce/cook/fs.h"
#include "mce/platform/library.h"
#include "mce/platform/process.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

namespace {

constexpr usize kMaxDiags = 200;
constexpr usize kMaxTailBytes = 4000;

struct BuildTree {
    cook::String dir;    // holds CMakeCache.txt
    cook::String cmake;  // CMAKE_COMMAND from the cache
    cook::String ctest;  // beside it
    cook::String target;  // <game>_game
    cook::String game;    // <game>
};

StringView parent_dir(StringView p) {
    usize sep = StringView::npos;
    for (usize i = 0; i < p.size(); ++i)
        if (p[i] == '/' || p[i] == '\\') sep = i;
    return sep == StringView::npos ? StringView() : p.substr(0, sep);
}

// The cache entry `key` ("CMAKE_COMMAND:INTERNAL=/usr/bin/cmake" -> "/usr/bin/cmake").
bool cache_value(StringView cache, StringView key, cook::String& out) {
    usize at = 0;
    while (at < cache.size()) {
        usize end = cache.find('\n', at);
        if (end == StringView::npos) end = cache.size();
        StringView line = cache.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line = line.substr(0, line.size() - 1);
        if (line.size() > key.size() && line.substr(0, key.size()) == key && line[key.size()] == ':') {
            const usize eq = line.find('=');
            if (eq != StringView::npos) {
                out = cook::String(line.substr(eq + 1));
                return true;
            }
        }
        at = end + 1;
    }
    return false;
}

bool find_build_tree(ToolCall& c, BuildTree& t) {
    if (!c.server.module().loaded())
        return fail(c, "no game module is loaded: start the stock server with --module <lib<game>.so> (mce mcp does); a <game>_mcp "
                       "build has the game compiled in and cannot rebuild it while it runs");
    const StringView module = c.server.module().source_path.view();
    // lib<game>.so / <game>.dll / lib<game>.dylib -> <game>
    StringView file = module.substr(parent_dir(module).size() + (parent_dir(module).empty() ? 0 : 1));
    if (file.size() > 3 && file.substr(0, 3) == "lib") file = file.substr(3);
    const usize dot = file.find('.');
    t.game = cook::String(dot == StringView::npos ? file : file.substr(0, dot));
    t.target = t.game;
    t.target += "_game";
    for (StringView dir = parent_dir(module); !dir.empty(); dir = parent_dir(dir)) {
        const cook::String cache_path = cook::path_join(dir, "CMakeCache.txt");
        cook::String cache;
        if (!cook::fs::read_text(cache_path.view(), cache)) continue;
        t.dir = cook::String(dir);
        if (!cache_value(cache.view(), "CMAKE_COMMAND", t.cmake)) return fail(c, "CMakeCache.txt has no CMAKE_COMMAND");
        t.ctest = cook::String(parent_dir(t.cmake.view()));
#if defined(_WIN32)
        t.ctest += "/ctest.exe";
#else
        t.ctest += "/ctest";
#endif
        return true;
    }
    c.error += "no CMake build tree above the module ";
    c.error += module;
    c.error += " (module_build rebuilds the tree the module was built in)";
    return false;
}

// Runs `args` to completion, capturing stdout and stderr.
bool run_captured(ToolCall& c, Span<const StringView> args, Array<char>& output, i32& code) {
    platform::Process proc;
    if (Result<void> r = proc.start(args, true); !r) {
        c.error += "cannot start ";
        c.error += args[0];
        return false;
    }
    code = proc.wait(&output);
    return true;
}

bool is_digits(StringView s) {
    if (s.empty()) return false;
    for (char ch : s)
        if (ch < '0' || ch > '9') return false;
    return true;
}

u32 to_u32(StringView s) {
    u32 v = 0;
    for (char ch : s) v = v * 10 + static_cast<u32>(ch - '0');
    return v;
}

struct CompilerDiag {
    StringView file, severity, message;
    u32 line = 0, col = 0;
};

// One compiler line: GCC/Clang "file:line:col: error: message" (or "file:line: ..."), MSVC
// "file(line,col): error C2065: message". False for anything else.
bool parse_diag_line(StringView line, CompilerDiag& d) {
    static constexpr StringView kSeverities[] = {"fatal error", "error", "warning", "note"};
    for (StringView sev : kSeverities) {
        cook::String gcc(": ");
        gcc += sev;
        gcc += ": ";
        usize at = line.find(gcc.view());
        if (at != StringView::npos) {
            // file:line[:col] before it
            const StringView head = line.substr(0, at);
            const usize c2 = head.rfind(':');
            if (c2 == StringView::npos) continue;
            const usize c1 = head.substr(0, c2).rfind(':');
            if (c1 != StringView::npos && c1 > 1 && is_digits(head.substr(c1 + 1, c2 - c1 - 1)) && is_digits(head.substr(c2 + 1))) {
                d.file = head.substr(0, c1);
                d.line = to_u32(head.substr(c1 + 1, c2 - c1 - 1));
                d.col = to_u32(head.substr(c2 + 1));
            } else if (is_digits(head.substr(c2 + 1))) {
                d.file = head.substr(0, c2);
                d.line = to_u32(head.substr(c2 + 1));
            } else {
                continue;
            }
            d.severity = sev == "fatal error" ? StringView("error") : sev;
            d.message = line.substr(at + gcc.size());
            return !d.file.empty();
        }
        cook::String msvc("): ");
        msvc += sev;
        msvc += ' ';
        at = line.find(msvc.view());
        if (at != StringView::npos) {
            const StringView head = line.substr(0, at);
            const usize open = head.rfind('(');
            if (open == StringView::npos) continue;
            const StringView pos = head.substr(open + 1);
            const usize comma = pos.find(',');
            const StringView ln = comma == StringView::npos ? pos : pos.substr(0, comma);
            if (!is_digits(ln)) continue;
            d.file = head.substr(0, open);
            d.line = to_u32(ln);
            d.col = comma != StringView::npos && is_digits(pos.substr(comma + 1)) ? to_u32(pos.substr(comma + 1)) : 0;
            d.severity = sev == "fatal error" ? StringView("error") : sev;
            d.message = line.substr(at + msvc.size());
            return true;
        }
    }
    return false;
}

void write_tail(JsonWriter& w, const Array<char>& out) {
    const usize from = out.size() > kMaxTailBytes ? out.size() - kMaxTailBytes : 0;
    w.str(StringView(out.data() + from, out.size() - from));
}

}  // namespace

bool module_build(ToolCall& c) {
    BuildTree t;
    bool reload = true, warnings = false;
    StringView target;
    if (!arg_bool(c, "reload", reload) || !arg_bool(c, "warnings", warnings) || !arg_string(c, "target", target, false) ||
        !find_build_tree(c, t))
        return false;
    if (!target.empty()) t.target = cook::String(target);
    u64 size_before = 0, size_after = 0;
    i64 mtime_before = 0, mtime_after = 0;
    const StringView module = c.server.module().source_path.view();
    (void)cook::fs::stat(module, size_before, mtime_before);

    const StringView args[] = {t.cmake.view(), "--build", t.dir.view(), "--target", t.target.view()};
    Array<char> out;
    i32 code = -1;
    if (!run_captured(c, Span<const StringView>(args, 5), out, code)) return false;

    // Diagnostics, de-duplicated (a header's error repeats per translation unit).
    Array<CompilerDiag> diags;
    u32 errors = 0, warns = 0;
    const StringView text(out.data(), out.size());
    for (usize at = 0; at < text.size();) {
        usize end = text.find('\n', at);
        if (end == StringView::npos) end = text.size();
        StringView line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line = line.substr(0, line.size() - 1);
        at = end + 1;
        CompilerDiag d;
        if (!parse_diag_line(line, d)) continue;
        bool dup = false;
        for (const CompilerDiag& e : diags)
            dup = dup || (e.file == d.file && e.line == d.line && e.col == d.col && e.message == d.message);
        if (dup) continue;
        if (d.severity == "error") ++errors;
        else if (d.severity == "warning") ++warns;
        if (d.severity == "error" || (warnings && d.severity != "error") || (!diags.empty() && d.severity == "note" && diags.back().severity == "error"))
            if (diags.size() < kMaxDiags) diags.push_back(d);
    }

    (void)cook::fs::stat(module, size_after, mtime_after);
    const bool rebuilt = code == 0 && (mtime_after != mtime_before || size_after != size_before);
    cook::String note;
    bool reloaded = false;
    if (code == 0 && reload) {
        if (Result<void> r = c.server.reload_module(note); !r) {
            c.error += "the build succeeded but the module did not load: ";
            FixedString<512> msg;
            format_error(r.error(), msg);
            c.error += msg.view();
            if (!note.empty()) {
                c.error += " (";
                c.error += note.view();
                c.error += ')';
            }
            return false;
        }
        reloaded = true;
    }

    c.out.begin_object();
    c.out.key("ok").boolean(code == 0);
    c.out.key("exit_code").num_i(code);
    c.out.key("target").str(t.target.view());
    c.out.key("build_dir").str(t.dir.view());
    c.out.key("rebuilt").boolean(rebuilt);
    c.out.key("reloaded").boolean(reloaded);
    if (!note.empty()) c.out.key("note").str(note.view());
    c.out.key("errors").num_u(errors).key("warnings").num_u(warns);
    c.out.key("diagnostics").begin_array();
    for (const CompilerDiag& d : diags) {
        c.out.begin_object().key("severity").str(d.severity).key("file").str(d.file).key("line").num_u(d.line);
        c.out.key("col").num_u(d.col).key("message").str(d.message).end_object();
    }
    c.out.end_array();
    if (code != 0) {
        // Link errors and CMake failures carry no file:line: the end of the output names them.
        c.out.key("output_tail");
        write_tail(c.out, out);
    }
    c.out.end_object();
    return true;
}

bool module_test(ToolCall& c) {
    BuildTree t;
    StringView filter;
    if (!arg_string(c, "filter", filter, false) || !find_build_tree(c, t)) return false;
    cook::String regex;
    if (filter.empty()) {
        regex = cook::String("^");
        regex += t.game.view();
        regex += "_";
    } else {
        regex = cook::String(filter);
    }
    const StringView args[] = {t.ctest.view(), "--test-dir", t.dir.view(), "-R", regex.view(), "--output-on-failure", "--no-tests=error"};
    Array<char> out;
    i32 code = -1;
    if (!platform::file_exists(t.ctest.view())) return fail(c, "ctest is not beside the CMake that configured the build tree");
    if (!run_captured(c, Span<const StringView>(args, 7), out, code)) return false;

    // ctest's per-test lines: "  1/3 Test  #7: name ....   Passed    0.12 sec"
    c.out.begin_object();
    c.out.key("ok").boolean(code == 0);
    c.out.key("filter").str(regex.view());
    u32 passed = 0, failed = 0;
    c.out.key("tests").begin_array();
    const StringView text(out.data(), out.size());
    for (usize at = 0; at < text.size();) {
        usize end = text.find('\n', at);
        if (end == StringView::npos) end = text.size();
        const StringView line = text.substr(at, end - at);
        at = end + 1;
        // "Test", blanks (ctest pads the number to the widest), then '#'.
        usize hash = line.find("Test ");
        if (hash != StringView::npos) {
            usize p = hash + 4;
            while (p < line.size() && line[p] == ' ') ++p;
            hash = p < line.size() && line[p] == '#' ? p : StringView::npos;
        }
        const usize colon = hash == StringView::npos ? StringView::npos : line.find(": ", hash);
        if (colon == StringView::npos) continue;
        StringView rest = line.substr(colon + 2);
        const usize sp = rest.find(' ');
        const StringView name = sp == StringView::npos ? rest : rest.substr(0, sp);
        const bool ok = line.find("   Passed") != StringView::npos;
        const bool not_run = line.find("Not Run") != StringView::npos;
        ok ? ++passed : ++failed;
        c.out.begin_object().key("name").str(name).key("result").str(ok ? "passed" : not_run ? "not run" : "failed").end_object();
    }
    c.out.end_array();
    c.out.key("passed").num_u(passed).key("failed").num_u(failed);
    if (code != 0) {
        c.out.key("output_tail");
        write_tail(c.out, out);
    }
    c.out.end_object();
    return true;
}

}  // namespace mce::mcp
