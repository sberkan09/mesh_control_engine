#pragma once
// The JSON source asset kinds the editor can create from a template (M-ASSETNEW): extension, importer format and
// version, the field list asset_schema describes, and the starter object a new file begins from (format, version and
// guid are added). The MCP asset tools and the Assets panel's New menu share this one table. Keep it in step with
// the importers (src/engine/asset/cook/src/*_importer.cpp): the tests create every kind from its template and cook it.

#include "mce/types.h"

namespace mce::editor {

struct AssetKind {
    const char* type;  // the asset_list type name
    const char* ext;
    const char* format;
    u32 version;
    const char* summary;
    const char* fields;
    const char* tmpl;
};

Span<const AssetKind> asset_kinds();
// The kind whose extension ends `path` (".mcpart"), or null.
const AssetKind* asset_kind_for_extension(StringView path);
// By type name ("ParticlePreset"), extension (".mcpart") or extension without the dot ("mcpart").
const AssetKind* asset_kind_named(StringView name);

}  // namespace mce::editor
