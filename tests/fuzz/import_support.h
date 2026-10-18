// Shared by the fuzz cases that run a cook importer over bytes they build themselves (test_mesh_import.cpp,
// test_defaults.cpp): no project, so every reference dangles.
#pragma once

#include <string>

#include "mce/asset/container.h"
#include "mce/cook/importer.h"

namespace mce::fuzz {

class NoLookup final : public cook::IAssetLookup {
public:
    bool type_of(cook::Guid, cook::ChunkType&) const override { return false; }
    bool guid_for_path(StringView, cook::Guid&) const override { return false; }
};

// scan_dependencies, then cook, of `bytes` as `rel`. True when the importer accepted it; the container goes to `out` when given.
inline bool import_bytes(const cook::IImporter& imp, const std::string& rel, Span<const byte> bytes, asset::ContainerWriter* out = nullptr) {
    cook::ImportInput in;
    const std::string disp = "assets/" + rel;
    in.display_path = StringView(disp.c_str(), disp.size());
    in.source_path = StringView(rel.c_str(), rel.size());
    in.source = bytes;
    cook::JsonDoc doc;
    if (imp.native()) {
        cook::JsonError err;
        if (!doc.parse(StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size()), err)) return false;
        in.settings = doc.root();
    }
    static const NoLookup lookup;
    in.lookup = &lookup;
    cook::DiagList diags;
    Array<cook::DepRef> deps;
    imp.scan_dependencies(in, deps, diags);
    if (diags.error_count() != 0) return false;
    asset::ContainerWriter local;
    asset::ContainerWriter& w = out ? *out : local;
    return imp.cook(in, w, diags) && diags.error_count() == 0;
}

}  // namespace mce::fuzz
