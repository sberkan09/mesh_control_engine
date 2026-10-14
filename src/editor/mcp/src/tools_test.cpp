// test_run and playtest (ROADMAP M-AGENT step 3): .mctest scenarios and bot runs, judged without a
// person. A scenario is a JSON file under the project's tests/ folder:
//
//   {
//     "name": "reach the flag",
//     "scene": "scenes/level1.mcscene",            // default: the open scene (saved)
//     "ticks": 1800,                               // the most it may run
//     "input": "10-70 Move=1,0 Jump",              // scripted input, or:
//     "bot": {"kind": "walk_to", "actor": "Player", "target": "Flag", "plane": "x"},
//     "actor": "Player",                           // the feel report's entity (default: the bot's)
//     "until": {"alive": "Flag", "value": false},  // ends the run early when it holds
//     "death": {"entity": "Player", "field": "Health.hp", "<=": 0},
//     "assert": [{"feel": "finished_tick", ">": 0}, {"log_errors": 0},
//                {"entity": "Player", "field": "LocalTransform.position.x", ">": 10, "tick": 600}],
//     "trace": {"every": 30, "fields": [{"entity": "Player", "field": "LocalTransform.position"}]}
//   }
//
// The conditions and bots are agent_sim.h's. Every run is run_headless's (deterministic: the same
// file gives the same state_hash).

#include "agent_sim.h"
#include "mce/cook/fs.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

namespace {

constexpr u64 kDefaultTestTicks = 1800;
constexpr u64 kMaxTestTicks = 36000;
constexpr usize kMaxTests = 64;
constexpr usize kFailLogLines = 10;

struct TestSpec {
    cook::String name;
    cook::String file;   // the .mctest path under the project root ("" for an inline test)
    cook::String scene;  // under assets/
    asset::Guid guid;
    u64 ticks = kDefaultTestTicks;
    editor::SceneDocument doc;
    input::InputScript script;
    bool has_bot = false;
    BotConfig bot;
    bool has_actor = false;
    usize actor = 0;
    Array<Condition> checks;
    bool has_until = false;
    Condition until;
    bool has_death = false;
    Condition death;
    Trace trace;
    f32 teleport = 3.0f;
};

bool spec_fail(ToolCall& c, const TestSpec& t, StringView msg) {
    if (!t.file.empty()) {
        c.error += t.file.view();
        c.error += ": ";
    }
    c.error += msg;
    return false;
}

constexpr const char* kSpecKeys[] = {"name", "scene", "ticks", "input", "bot", "actor", "until", "death", "assert", "trace", "teleport",
                                     // playtest's own arguments (it reads its whole argument object as a scenario)
                                     "log_lines"};

bool parse_spec(ToolCall& c, const cook::JsonValue& v, TestSpec& t) {
    if (!v.is_object()) return spec_fail(c, t, "a scenario is a JSON object");
    for (u32 i = 0; i < v.size(); ++i) {
        bool known = false;
        for (const char* k : kSpecKeys) known = known || v.key_at(i) == k;
        if (!known) {
            cook::String msg("unknown key '");
            msg += v.key_at(i);
            msg += "' (name, scene, ticks, input, bot, actor, until, death, assert, trace, teleport)";
            return spec_fail(c, t, msg.view());
        }
    }
    const cook::JsonValue name = v.get("name"), scene_v = v.get("scene"), ticks = v.get("ticks");
    if (name.is_string()) t.name = cook::String(name.as_string());
    // The scene: the named file, or the open one (which must be saved: the run plays the cooked file).
    const Server::SceneFile& open = c.server.scene_file();
    if (scene_v.valid()) {
        if (!scene_v.is_string()) return spec_fail(c, t, "'scene' is a path under assets/");
        t.scene = cook::String(scene_v.as_string());
    } else {
        if (open.rel.empty()) return spec_fail(c, t, "no 'scene' and the open scene was never saved");
        t.scene = open.rel;
    }
    if (t.scene.view() == open.rel.view() && scene(c).dirty())
        return spec_fail(c, t, "the scene has unsaved changes: scene_save first (tests play the cooked file)");
    cook::String err;
    if (!load_scene_doc(c.server, t.scene.view(), t.doc, err)) return spec_fail(c, t, err.view());
    t.guid = t.doc.guid();
    for (const editor::ProjectAsset& a : c.server.session().project().assets())
        if (a.type == asset::ChunkType::Scene && a.path.view() == t.scene.view()) t.guid = a.guid;
    if (t.name.empty()) t.name = t.file.empty() ? cook::String(t.scene.view()) : t.file;
    if (ticks.valid()) {
        if (!ticks.is_integer() || ticks.as_number() < 1 || ticks.as_number() > static_cast<f64>(kMaxTestTicks))
            return spec_fail(c, t, "'ticks' is 1-36000");
        t.ticks = static_cast<u64>(ticks.as_number());
    }
    const cook::JsonValue bot = v.get("bot"), input = v.get("input");
    if (bot.valid() && input.valid()) return spec_fail(c, t, "give 'input' or 'bot', not both");
    if (input.valid()) {
        cook::String saved;
        saved = c.error;
        if (!input_from(c, input, t.script)) {
            cook::String msg(c.error.view().substr(saved.size()));
            c.error = saved;
            return spec_fail(c, t, msg.view());
        }
    }
    if (bot.valid()) {
        if (!parse_bot(t.doc, bot, t.bot, err)) return spec_fail(c, t, err.view());
        t.has_bot = true;
        t.has_actor = t.bot.has_actor;
        t.actor = t.bot.actor;
    }
    const cook::JsonValue actor = v.get("actor");
    if (actor.valid()) {
        if (!scene_entity_index(t.doc, actor, t.actor, err)) return spec_fail(c, t, err.view());
        t.has_actor = true;
    }
    const cook::JsonValue until = v.get("until"), death = v.get("death"), checks = v.get("assert"), trace = v.get("trace"),
                          teleport = v.get("teleport");
    if (until.valid()) {
        if (!parse_condition(t.doc, until, t.until, err)) return spec_fail(c, t, err.view());
        if (t.until.what != Condition::What::Field && t.until.what != Condition::What::Alive && t.until.what != Condition::What::Event)
            return spec_fail(c, t, "'until' checks the world: a field, alive or an event");
        t.has_until = true;
    }
    if (death.valid()) {
        if (!parse_condition(t.doc, death, t.death, err)) return spec_fail(c, t, err.view());
        if (t.death.what != Condition::What::Field && t.death.what != Condition::What::Alive)
            return spec_fail(c, t, "'death' checks the world: a field or alive");
        t.has_death = true;
    }
    if (checks.valid()) {
        if (!checks.is_array() || checks.size() > 64) return spec_fail(c, t, "'assert' is an array of up to 64 checks");
        for (u32 i = 0; i < checks.size(); ++i) {
            Condition cond;
            if (!parse_condition(t.doc, checks.at(i), cond, err)) {
                cook::String msg;
                cook::appendf(msg, "assert[%u]: ", i);
                msg += err.view();
                return spec_fail(c, t, msg.view());
            }
            if (cond.tick && cond.what != Condition::What::Field && cond.what != Condition::What::Alive && cond.what != Condition::What::Event)
                return spec_fail(c, t, "'tick' is for world checks (field, alive, event); log and feel checks look at the whole run");
            t.checks.push_back(static_cast<Condition&&>(cond));
        }
    }
    if (trace.valid() && !parse_trace(t.doc, trace, t.trace, err)) return spec_fail(c, t, err.view());
    if (teleport.valid()) {
        if (!teleport.is_number() || teleport.as_number() <= 0) return spec_fail(c, t, "'teleport' is a distance > 0 (metres per tick)");
        t.teleport = static_cast<f32>(teleport.as_number());
    }
    return true;
}

struct EndChecks {
    TestSpec* spec;
};

// Runs one scenario and writes its result object. Returns false only when the run could not
// start (c.error set); a failing check is a result (ok: false).
bool run_spec(ToolCall& c, TestSpec& t, StringView cooked, bool& passed) {
    Bot bot(t.bot);
    FeelTracker feel;
    feel.teleport = t.teleport;
    SimRun sim;
    sim.bot = t.has_bot ? &bot : nullptr;
    sim.script = t.has_bot || t.script.empty() ? nullptr : &t.script;
    sim.trace = t.trace.active() ? &t.trace : nullptr;
    sim.feel = &feel;
    sim.has_actor = t.has_actor;
    sim.actor = t.actor;
    sim.checks = &t.checks;
    sim.until = t.has_until ? &t.until : nullptr;
    sim.death = t.has_death ? &t.death : nullptr;
    const RunHooks hooks = sim.hooks();
    app::IInputSource* input = sim.bot ? static_cast<app::IInputSource*>(&bot) : (t.script.empty() ? nullptr : &t.script);

    EndChecks end{&t};
    const u64 since = log::console_last_seq();
    Result<HeadlessRun> run = run_headless(
        cooked, t.guid, t.ticks, nullptr, nullptr,
        [](const World& world, Span<const Entity> spawned, void* user) {
            for (Condition& cond : static_cast<EndChecks*>(user)->spec->checks)
                if (cond.tick == 0) eval_world(cond, world, spawned);
        },
        &end, input, c.server.session().project().base_roots(), &hooks);
    Array<log::ConsoleLine> lines;
    read_log(since, lines);

    c.out.begin_object();
    c.out.key("name").str(t.name.view());
    if (!t.file.empty()) c.out.key("file").str(t.file.view());
    c.out.key("scene").str(t.scene.view());
    if (!run) {
        FixedString<512> msg;
        format_error(run.error(), msg);
        c.out.key("ok").boolean(false).key("error").str(msg.view());
        c.out.key("log");
        write_log_lines(c.out, lines.span(), kFailLogLines);
        c.out.end_object();
        passed = false;
        return true;
    }
    passed = true;
    for (Condition& cond : t.checks) {
        if (cond.tick == 0) eval_after(cond, lines.span(), &feel);
        passed = passed && cond.checked && cond.ok;
    }
    const HeadlessRun& h = run.value();
    c.out.key("ok").boolean(passed);
    c.out.key("ticks").num_u(h.ticks);
    c.out.key("stopped_early").boolean(sim.stopped_early);
    cook::String hash;
    cook::appendf(hash, "%016llx", static_cast<unsigned long long>(h.state_hash));
    c.out.key("state_hash").str(hash.view());
    if (t.has_bot) c.out.key("bot").str(bot_kind_name(t.bot.kind));
    c.out.key("checks").begin_array();
    for (const Condition& cond : t.checks) write_condition(c.out, cond);
    c.out.end_array();
    if (t.has_actor) {
        c.out.key("feel");
        feel.write(c.out, h.ticks);
    }
    if (sim.trace) {
        c.out.key("trace");
        t.trace.write(c.out);
    }
    u32 errors = 0, warnings = 0;
    for (const log::ConsoleLine& l : lines) {
        if (l.level == log::Level::Warn) ++warnings;
        if (l.level >= log::Level::Error && l.level != log::Level::Off) ++errors;
    }
    c.out.key("log").begin_object().key("warnings").num_u(warnings).key("errors").num_u(errors);
    if (!passed) {
        c.out.key("tail");
        write_log_lines(c.out, lines.span(), kFailLogLines);
    }
    c.out.end_object();
    c.out.end_object();
    return true;
}

bool cook_for_tests(ToolCall& c, cook::String& cooked) {
    const cook::CookReport& r = c.server.session().cook(false);
    if (!r.ok()) {
        c.error += "cook failed:";
        append_diags(c.error, r.diags, 10);
        return false;
    }
    cooked = cook::path_join(c.server.session().project().root(), "cooked");
    return true;
}

bool ends_with(StringView s, StringView suffix) { return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix; }

// Reads and parses one .mctest file (`rel` under the project root).
bool load_spec(ToolCall& c, StringView rel, TestSpec& t) {
    t.file = cook::String(rel);
    const cook::String abs = cook::path_join(c.server.session().project().root(), rel);
    cook::String text;
    if (!cook::fs::read_text(abs.view(), text)) return spec_fail(c, t, "cannot read the file");
    cook::JsonDoc doc;
    cook::JsonError jerr;
    if (!doc.parse(text.view(), jerr)) {
        cook::String msg;
        cook::appendf(msg, "%u:%u: %s", jerr.line, jerr.col, jerr.message.c_str());
        return spec_fail(c, t, msg.view());
    }
    return parse_spec(c, doc.root(), t);
}

bool safe_rel(StringView p) {
    if (p.empty() || p[0] == '/' || p[0] == '\\' || p.find(':') != StringView::npos) return false;
    usize at = 0;
    while (at <= p.size()) {
        usize end = at;
        while (end < p.size() && p[end] != '/' && p[end] != '\\') ++end;
        if (p.substr(at, end - at) == "..") return false;
        at = end + 1;
    }
    return true;
}

}  // namespace

bool test_run(ToolCall& c) {
    if (!need_project(c)) return false;
    StringView path;
    if (!arg_string(c, "path", path, false)) return false;
    const cook::JsonValue inline_test = arg(c, "test");
    if (!path.empty() && inline_test.valid()) return fail(c, "give 'path' or 'test', not both");
    if (!path.empty() && !safe_rel(path)) return fail(c, "argument 'path': a file or folder inside the project, e.g. tests/jump.mctest");

    // Which scenarios: one inline, one file, or every .mctest under a folder (default tests/).
    Array<cook::String> files;
    if (!inline_test.valid()) {
        const StringView dir = path.empty() ? StringView("tests") : path;
        const cook::String abs = cook::path_join(c.server.session().project().root(), dir);
        if (cook::fs::is_directory(abs.view())) {
            Array<cook::String> all;
            if (Result<void> r = cook::fs::list_files(abs.view(), all); !r) return fail(c, "cannot list the tests folder");
            for (const cook::String& f : all)
                if (ends_with(f.view(), ".mctest")) files.push_back(cook::path_join(dir, f.view()));
            if (files.empty()) {
                cook::String msg("no .mctest files under ");
                msg += dir;
                msg += "/ (the user guide's chapter 14 shows the format)";
                return fail(c, msg.view());
            }
            if (files.size() > kMaxTests) return fail(c, "more than 64 scenarios: give a sub-folder or one file");
        } else if (cook::fs::exists(abs.view())) {
            files.push_back(cook::String(dir));
        } else {
            cook::String msg("no such file or folder in the project: ");
            msg += dir;
            return fail(c, msg.view());
        }
    }

    cook::String cooked;
    if (!cook_for_tests(c, cooked)) return false;
    u32 passed = 0, failed = 0;
    c.out.begin_object();
    c.out.key("results").begin_array();
    const usize count = inline_test.valid() ? 1 : files.size();
    for (usize i = 0; i < count; ++i) {
        TestSpec t;
        const bool parsed = inline_test.valid() ? parse_spec(c, inline_test, t) : load_spec(c, files[i].view(), t);
        if (!parsed) return false;  // a bad scenario is the caller's to fix: say which and why
        bool ok = false;
        if (!run_spec(c, t, cooked.view(), ok)) return false;
        (ok ? passed : failed) += 1;
    }
    c.out.end_array();
    c.out.key("passed").num_u(passed).key("failed").num_u(failed).key("ok").boolean(failed == 0);
    c.out.end_object();
    return true;
}

bool playtest(ToolCall& c) {
    if (!need_project(c)) return false;
    if (!arg(c, "bot").valid()) return arg_missing(c, "bot", "an object: {\"kind\": \"walk_to\" | \"explore\" | \"masher\", \"actor\": ...}");
    TestSpec t;
    if (!parse_spec(c, c.args, t)) return false;
    cook::String cooked;
    if (!cook_for_tests(c, cooked)) return false;
    bool ok = false;
    return run_spec(c, t, cooked.view(), ok);
}

}  // namespace mce::mcp
