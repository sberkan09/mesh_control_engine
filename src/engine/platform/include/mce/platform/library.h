#pragma once
// Shared-library loading and file copy (core.md §2.5 `load_library`). The game-module loader
// (scripting.md §2.2) is the consumer; nothing here needs platform::init().

#include "mce/result.h"

namespace mce::platform {

// Maps a shared library (RTLD_NOW | RTLD_LOCAL on ELF). Errc::NotFound if the file is missing,
// Errc::IoError if the OS refuses it (bad image, unresolved symbols); the OS message is logged.
Result<void*> load_library(StringView path);
// Null when the library does not export `name`.
void* library_symbol(void* library, const char* name);
void unload_library(void* library);

bool file_exists(StringView path);
// Copies `from` over `to` (replacing it). Errc::FileNotFound / Errc::IoError.
ResultV copy_file(StringView from, StringView to);
// Creates `path` and every missing parent (no-op when it exists). Errc::IoError.
ResultV create_directories(StringView path);

}  // namespace mce::platform
