// mce_script_compiler: Luau's parser for positioned syntax errors, then luau_compile for the
// bytecode. The Luau C++ libraries throw internally and catch their own exceptions (Parser::parse
// and luau_compile both do), so this -fno-exceptions TU only sees results.

#include "mce/script/compiler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Luau/Allocator.h"
#include "Luau/Ast.h"
#include "Luau/Lexer.h"
#include "Luau/Parser.h"
#include "luacode.h"

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

Result<Array<u8>> compile(StringView name, StringView source, CompileError* error) {
    {  // Syntax first: the parser knows the column, luau_compile's message only the line.
        Luau::Allocator allocator;
        Luau::AstNameTable names(allocator);
        const Luau::ParseResult parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator);
        if (!parsed.errors.empty()) {
            const Luau::ParseError& e = parsed.errors.front();
            const Luau::Location& at = e.getLocation();
            return compile_failure(name, at.begin.line + 1, at.begin.column + 1, StringView(e.getMessage().c_str()), error);
        }
    }
    lua_CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;  // line info for error messages
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

Result<u32> add_source(ScriptHost& host, StringView name, StringView source, u8 group) {
    auto bc = compile(name, source);
    if (!bc) return bc.error();
    return host.add_bytecode(name, bc.value().span(), group);
}

}  // namespace mce::script
