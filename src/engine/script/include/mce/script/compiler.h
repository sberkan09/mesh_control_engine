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

// Luau source -> bytecode. Fails with Errc::ParseError and "<name>:<line>:<col>: message" in path;
// `error`, when given, gets the position and the bare message.
Result<Array<u8>> compile(StringView name, StringView source, CompileError* error = nullptr);

// compile() then host.add_bytecode(): the source path for tests, samples and tools.
Result<u32> add_source(ScriptHost& host, StringView name, StringView source, u8 group = 0);

}  // namespace mce::script
