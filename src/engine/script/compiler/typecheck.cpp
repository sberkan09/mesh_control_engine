// mce_script_compiler (M-SCRIPT 4): see typecheck.h.

#include "typecheck.h"

#include <optional>

#include "Luau/BuiltinDefinitions.h"
#include "Luau/ConfigResolver.h"
#include "Luau/Frontend.h"
#include "Luau/ToString.h"
#include "mce/script/compiler.h"

namespace mce::script::detail {

namespace {

// One in-memory module: the script being checked.
struct OneFile final : Luau::FileResolver {
    StringView source;
    std::optional<Luau::SourceCode> readSource(const Luau::ModuleName&) override {
        return Luau::SourceCode{{source.data(), source.size()}, Luau::SourceCode::Script};
    }
};

void push(Array<NameIssue>& out, const Luau::Location& at, const char* msg) {
    NameIssue i;
    i.line = at.begin.line + 1;
    i.col = at.begin.column + 1;
    i.message.assign(StringView(msg));
    out.push_back(i);
}

}  // namespace

void check_types(StringView name, StringView source, Array<NameIssue>& out) {
    Array<char> defs;
    write_definitions(defs);
    OneFile files;
    files.source = source;
    Luau::NullConfigResolver config;  // Nonstrict unless the script says --!strict
    Luau::FrontendOptions options;
    options.moduleTimeLimitSec = 10.0;
    try {
        Luau::Frontend frontend(Luau::SolverMode::Old, &files, &config, options);
        Luau::registerBuiltinGlobals(frontend, frontend.globals);
        const Luau::LoadDefinitionFileResult loaded = frontend.loadDefinitionFile(
            frontend.globals, frontend.globals.globalScope, {defs.data(), defs.size()}, "@mce", false, false);
        if (!loaded.success) {
            push(out, Luau::Location{}, "internal: mce.d.luau does not load (report this as an engine bug)");
            return;
        }
        Luau::freeze(frontend.globals.globalTypes);
        const Luau::CheckResult result = frontend.check(Luau::ModuleName(name.data(), name.size()));
        for (const Luau::TypeError& e : result.errors) {
            if (Luau::get_if<Luau::SyntaxError>(&e.data)) continue;  // the parser already reported it
            const auto text = Luau::toString(e);
            push(out, e.location, text.c_str());
        }
    } catch (...) {
        push(out, Luau::Location{}, "the type checker gave up on this script (too large or too complex)");
    }
}

}  // namespace mce::script::detail
