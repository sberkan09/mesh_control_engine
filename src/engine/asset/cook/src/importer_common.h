#pragma once
// Shared helpers for the built-in importers (cook-internal).

#include "mce/cook/importer.h"

namespace mce::cook::detail {

// Reports keys of `obj` not in `allowed` as errors (typos must not silently fall back to defaults).
bool check_keys(JsonValue obj, std::initializer_list<StringView> allowed, StringView file, const char* stage, DiagList& diags);

// Typed setting reads with defaults; a present value of the wrong type is an error.
bool read_bool(JsonValue obj, StringView key, bool def, bool& out, StringView file, const char* stage, DiagList& diags);
bool read_number(JsonValue obj, StringView key, f64 def, f64& out, StringView file, const char* stage, DiagList& diags);

// The "version" every asset text carries: an integer from 1 up to `current`. A file older than `current` is read by the
// current code (a format bump adds its migration there and keeps its old files loading); anything else reports
// "unsupported <what> version (this cook reads version N)" with Errc::VersionTooNew and returns false.
bool check_format_version(JsonValue doc, u32 current, const char* what, StringView file, const char* stage, DiagList& diags);

void error_at(DiagList& diags, Errc code, StringView file, JsonValue at, const char* stage, StringView msg);

}  // namespace mce::cook::detail
