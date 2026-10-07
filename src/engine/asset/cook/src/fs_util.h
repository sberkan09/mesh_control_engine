#pragma once
// Cook-side filesystem helpers (std::filesystem error_code overloads only: no exceptions).

#include "mce/cook/string.h"
#include "mce/result.h"

namespace mce::cook::fs {

bool exists(StringView path);
bool is_directory(StringView path);
Result<void> create_directories(StringView path);
// Writes to "<path>.tmp" then renames over `path` (replace-existing on every platform), creating
// parent directories. A crash mid-write leaves the previous file intact.
Result<void> write_file_atomic(StringView path, Span<const byte> bytes);
Result<void> rename(StringView from, StringView to);
// Regular files under `root`, as paths relative to it with forward slashes, sorted bytewise.
// Entries whose name starts with '.' (and everything below them) are skipped.
Result<void> list_files(StringView root, Array<String>& out);

}  // namespace mce::cook::fs
