#pragma once
// mce_script_compiler (M-SCRIPT 1): Luau source -> bytecode. Linked by the cook, the editor and the
// tests; never by the player, which loads the bytecode mce_cook writes into CHNK_SCRIPT (the
// "runtime parses no text" rule and the compiler wall in scripts/ci/check_walls.py).

#include "mce/result.h"
#include "mce/script/script.h"

namespace mce::script {

// Where a compile failed: 1-based line and column (column 0 when only the line is known).
struct CompileError {
    u32 line = 0;
    u32 col = 0;
    FixedString<256> message;  // without the name and position
};

// Every problem in a script, in source order: a syntax error (nothing else is checked then), state
// kept outside components (M-SCRIPT 3), component / event / field names the engine does not know
// and sandbox-removed globals, then Luau's type errors against mce.d.luau. Empty: the script is fine.
void check(StringView name, StringView source, Array<CompileError>& out);

// Luau source -> bytecode, after check(). Fails with Errc::ParseError and
// "<name>:<line>:<col>: message" (the first problem) in path; `error`, when given, gets the
// position and the bare message.
Result<Array<u8>> compile(StringView name, StringView source, CompileError* error = nullptr);

// luau_compile alone, with no check(): for tests of what the run-time sandbox stops on its own
// (the cook and the tools always check).
Result<Array<u8>> compile_unchecked(StringView name, StringView source);

// mce.d.luau: the `mce` API as Luau type definitions, with the reflected components, events and
// fields (sorted, so the text depends only on the registry). check() uses it; mce_cook writes it to
// <project>/cooked/mce.d.luau for editors.
void write_definitions(Array<char>& out);

// compile() then host.add_bytecode(): the source path for tests, samples and tools.
Result<u32> add_source(ScriptHost& host, StringView name, StringView source, u8 group = 0);

}  // namespace mce::script
