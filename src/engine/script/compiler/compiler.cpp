// mce_script_compiler: check() (Luau's parser for positioned syntax errors, the hidden-state,
// name and type checks), then luau_compile for the bytecode. Parser::parse and luau_compile throw
// internally and catch their own exceptions, so this -fno-exceptions TU only sees results.

#include "mce/script/compiler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Luau/Allocator.h"
#include "Luau/Ast.h"
#include "Luau/Lexer.h"
#include "Luau/Parser.h"
#include "luacode.h"
#include "names_check.h"
#include "state_check.h"
#include "typecheck.h"

namespace mce::script {

namespace {

Error compile_failure(StringView name, u32 line, u32 col, StringView message, CompileError* error) {
    if (error) {
        error->line = line;
        error->col = col;
        error->message.assign(message);
    }
    FixedString<256> msg(name);
    char pos[32];
    if (col) std::snprintf(pos, sizeof(pos), ":%u:%u: ", line, col);
    else std::snprintf(pos, sizeof(pos), ":%u: ", line);
    msg.append(pos);
    msg.append(message);
    return Error(Errc::ParseError, "script.compile", msg.view());
}

}  // namespace

void check(StringView name, StringView source, Array<CompileError>& out) {
    out.clear();
    Array<detail::NameIssue> issues;
    {
        Luau::Allocator allocator;
        Luau::AstNameTable names(allocator);
        const Luau::ParseResult parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator);
        if (!parsed.errors.empty()) {  // the parser knows the column; luau_compile's message only the line
            const Luau::ParseError& e = parsed.errors.front();
            const Luau::Location& at = e.getLocation();
            CompileError c;
            c.line = at.begin.line + 1;
            c.col = at.begin.column + 1;
            c.message.assign(StringView(e.getMessage().c_str()));
            out.push_back(c);
            return;
        }
        detail::StateIssue state;
        if (!detail::check_state(parsed.root, state)) issues.push_back(state);
        detail::check_names(parsed.root, issues);
    }
    detail::check_types(name, source, issues);
    // Source order; at one position the first reported (the engine's checks before Luau's) wins.
    for (usize i = 1; i < issues.size(); ++i)
        for (usize j = i; j > 0 && (issues[j].line < issues[j - 1].line ||
                                    (issues[j].line == issues[j - 1].line && issues[j].col < issues[j - 1].col));
             --j) {
            const detail::NameIssue tmp = issues[j];
            issues[j] = issues[j - 1];
            issues[j - 1] = tmp;
        }
    for (const detail::NameIssue& i : issues) {
        if (!out.empty() && out.back().line == i.line && out.back().col == i.col) continue;
        CompileError c;
        c.line = i.line;
        c.col = i.col;
        c.message = i.message;
        out.push_back(c);
    }
}

namespace {

// luau_compile; its errors carry only the line.
Result<Array<u8>> bytecode(StringView name, StringView source, CompileError* error) {
    lua_CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 2;  // line info and local and upvalue names, for error messages (the state check names upvalues)
    usize size = 0;
    char* bc = luau_compile(source.data(), source.size(), &opts, &size);
    if (!bc) return Error(Errc::OutOfMemory, "script.compile", name);
    // A failed compile returns bytecode whose first byte is 0, followed by ":<line>: message".
    if (size == 0 || bc[0] == 0) {
        StringView text(size > 1 ? bc + 1 : "", size > 1 ? size - 1 : 0);
        u32 line = 0;
        if (!text.empty() && text[0] == ':') {
            usize i = 1;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') line = line * 10 + u32(text[i++] - '0');
            if (i < text.size() && text[i] == ':') ++i;
            while (i < text.size() && text[i] == ' ') ++i;
            text = text.substr(i);
        }
        Error err = compile_failure(name, line, 0, text, error);
        std::free(bc);
        return err;
    }
    Array<u8> out;
    out.resize(size);
    std::memcpy(out.data(), bc, size);
    std::free(bc);
    return out;
}

}  // namespace

Result<Array<u8>> compile(StringView name, StringView source, CompileError* error) {
    {
        Array<CompileError> problems;
        check(name, source, problems);
        if (!problems.empty())
            return compile_failure(name, problems[0].line, problems[0].col, problems[0].message.view(), error);
    }
    return bytecode(name, source, error);
}

Result<Array<u8>> compile_unchecked(StringView name, StringView source) { return bytecode(name, source, nullptr); }

Result<u32> add_source(ScriptHost& host, StringView name, StringView source, u8 group) {
    auto bc = compile(name, source);
    if (!bc) return bc.error();
    return host.add_bytecode(name, bc.value().span(), group);
}

}  // namespace mce::script
