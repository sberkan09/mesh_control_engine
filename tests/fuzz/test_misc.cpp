// M-FUZZ 3c: the command line and the console (the player types or passes them), and .mcperf captures (tools read
// them back). Nothing here may crash, hang or fire an MCE_ASSERT on any text.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "mce/convar.h"
#include "mce/log.h"
#include "mce/perf_capture.h"
#include "json_swap.h"
#include "mutator.h"
#include "test_support.h"
#if defined(MCE_FUZZ_HAS_EDITOR_CORE)
#include "mce/editor/perf_file.h"
#endif

using namespace mce;

namespace {

u32 iterations(u32 base) {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : base;
}

}  // namespace

TEST_CASE("fuzz: the command line and console lines survive hostile text") {
    log::set_level(log::Level::Off);
    // Names the parser knows (every registered convar and function) mixed with junk, so values reach the typed parsers.
    std::vector<std::string> names;
    {
        Array<ConVar*> vars;
        ConVar::collect(vars);
        for (ConVar* v : vars) names.push_back(v->name());
        Array<ConFunc*> funcs;
        ConFunc::collect(funcs);
        for (ConFunc* f : funcs) names.push_back(f->name());
    }
    REQUIRE(!names.empty());
    const char* junk[] = {"",       "-",      "--",     "-=",   "=",     "-help", "-1",      "nan",      "inf",       "1e999", "-0",
                          "999999999999999999999999",   "true", "false", "\"",    "\\",       "\xff\xfe", "a b",       "0x10",  "1,5",
                          "+",     "-name",  "--name", "-x=1", "name=1", "\t",   "\n",       "-seed",    "-quit"};
    fuzz::Mutator m(0xC0DE1);
    const u32 n = iterations(4000);
    u64 applied = 0;
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: command line iteration " << it);
        std::vector<std::string> args = {"app"};
        const u32 count = static_cast<u32>(m.below(7));
        for (u32 i = 0; i < count; ++i) {
            const u64 pick = m.below(3);
            std::string a;
            if (pick == 0) a = "-" + names[m.below(names.size())];
            else if (pick == 1) a = junk[m.below(sizeof(junk) / sizeof(junk[0]))];
            else a = "-" + names[m.below(names.size())] + (m.below(2) ? "=" : "") + junk[m.below(sizeof(junk) / sizeof(junk[0]))];
            args.push_back(a);
        }
        std::vector<const char*> argv;
        for (const std::string& s : args) argv.push_back(s.c_str());
        applied += console::parse_command_line(static_cast<int>(argv.size()), argv.data(), m.below(2) == 0);
        console::apply_deferred_command_line();
        // A console line: the same pieces joined.
        std::string line;
        for (size_t i = 1; i < args.size(); ++i) line += (i > 1 ? " " : "") + args[i].substr(args[i].size() && args[i][0] == '-' ? 1 : 0);
        (void)console::execute(StringView(line.c_str(), line.size()));
    }
    std::printf("fuzz: command line: %u lines, %llu unknown entries\n", n, static_cast<unsigned long long>(applied));
    CHECK(n > 0);
}

#if defined(MCE_FUZZ_HAS_EDITOR_CORE)
namespace {

perf::Capture sample_capture() {
    perf::Capture c;
    c.app = "fuzz";
    c.backend = "headless";
    c.scene = "scenes/main.mcscene";
    c.width = 1280;
    c.height = 720;
    c.sim_hz = 60;
    c.phases.push_back(FixedString<32>("update"));
    c.phases.push_back(FixedString<32>("render"));
    c.render_phases = 2;
    c.gpu_stages.push_back(FixedString<32>("shadow"));
    for (u32 i = 0; i < 12; ++i) {
        perf::Frame f;
        f.tick = i;
        f.frame_ms = 4.0f + static_cast<f32>(i % 5);
        c.frames.push_back(f);
    }
    perf::SystemStats s;
    s.name = "movement";
    s.runs = 12;
    s.total_ms = 3.5;
    c.systems.push_back(s);
    return c;
}

}  // namespace

TEST_CASE("fuzz: .mcperf captures survive mutated text") {
    log::set_level(log::Level::Off);
    const perf::Capture good = sample_capture();
    Array<char> text;
    perf::write_mcperf(good, perf::summarize(good), text);
    REQUIRE(!text.empty());
    {  // the untouched file reads back
        perf::Capture back;
        cook::String err;
        REQUIRE(editor::read_mcperf(StringView(text.data(), text.size()), back, err));
        CHECK(back.frames.size() == good.frames.size());
    }
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0xBEEF9);
    const u32 n = iterations(3000);
    u64 accepted = 0;
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: mcperf iteration " << it);
        const Span<const byte> bytes(reinterpret_cast<const byte*>(text.data()), text.size());
        std::string swapped;
        const char* p;
        usize len;
        if (it % 2 == 1) {
            swapped = fuzz::swap_value(m, bytes);
            p = swapped.data();
            len = swapped.size();
        } else {
            len = m.mutate(bytes, storage);
            p = reinterpret_cast<const char*>(storage.data());
        }
        perf::Capture back;
        cook::String err;
        if (editor::read_mcperf(StringView(p, len), back, err)) {
            ++accepted;
            (void)perf::summarize(back);  // what the tools do next
        }
    }
    std::printf("fuzz: mcperf: %u mutants, %llu read\n", n, static_cast<unsigned long long>(accepted));
    CHECK(accepted > 0);
}
#endif
