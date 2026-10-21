#pragma once
// The .mcui source format (M-UI-DATA step 1): read by the `ui` importer, written by tools that
// save a ui::DocumentData (mce/ui/document_data.h). The format is documented in ui_importer.cpp
// and user guide 12.
//
// Writing emits every widget with "kind_defaults": false and only the values that differ from
// the C++ defaults, so reading what was written gives back the same DocumentData, and cooking it
// the same CHNK_UIDOC bytes.

#include "mce/asset/guid.h"
#include "mce/cook/diagnostics.h"
#include "mce/cook/json.h"

namespace mce::ui {
struct DocumentData;
struct ThemeData;
}

namespace mce::cook {

inline constexpr u32 kUiFormatVersion = 1;
inline constexpr u32 kUiThemeFormatVersion = 1;  // .mcuitheme (M-UI-DATA step 3; format in ui_importer.cpp)

// Parses a .mcui document root into `out`. False when any diagnostic was added.
bool read_ui_document(JsonValue doc, StringView file, ui::DocumentData& out, DiagList& diags);

// Parses a .mcuitheme root into `out` (starting from ui::default_theme). False on any diagnostic.
bool read_ui_theme(JsonValue doc, StringView file, ui::ThemeData& out, DiagList& diags);

// Appends `d` as .mcui source text (two-space indented, a trailing newline).
void write_ui_document(const ui::DocumentData& d, asset::Guid guid, String& out);

}  // namespace mce::cook
