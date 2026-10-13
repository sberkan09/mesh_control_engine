#pragma once
// String-table CSV (M-MOBILE step 11a; mce/loc/string_table.h has the format), shared by the
// string importer and the cook's missing-key check (cook-internal).
//
//   key,text            <- header row; more columns ("note", ...) are allowed and ignored
//   menu.play,Play
//   menu.greeting,"Hello, {name}!"
//   score.coins#one,{count} coin
//   score.coins#other,{count} coins
//   # a comment row
//
// RFC 4180 quoting ("" inside quotes is one "; a quoted field may span lines), UTF-8 with an
// optional BOM, \n or \r\n line ends. A key with plural forms needs its #other form.

#include "mce/cook/diagnostics.h"
#include "mce/loc/string_table.h"

namespace mce::cook::detail {

// Parses `text` into `out` (keys and texts; the locale is the caller's). Every problem is an
// error in `diags` with its line; false when there was one.
bool parse_string_csv(StringView text, StringView file, loc::StringTable& out, DiagList& diags);

// The locale a string table's file names: "loc/tr-TR.csv" -> "tr-TR". False when the file stem
// is not a language tag.
bool locale_from_path(StringView source_path, loc::LocaleTag& out);

}  // namespace mce::cook::detail
