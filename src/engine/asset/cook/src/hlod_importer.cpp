// .mchlod -> CHNK_MESH: an HLOD proxy (M-OPENWORLD A3), a mesh the hlod_bake tool merged from a streamed area's
// static meshes (render::build_merged_mesh). The file is the CHNK_MESH payload as baked between a small header and
// a trailing hash (the .mcnav precedent); the cook checks both, validates the payload with MeshView::read and emits
// it as an ordinary mesh, so the runtime draws it like any other. The submeshes' texture GUIDs are declared as
// links: the textures must exist, but the proxy's bytes do not depend on theirs.
//
// Sidecar settings: "lod_levels" (0..3, default 0), "lod_ratio" (0.35), "lod_error" (0.05), as for glTF. With
// lod_levels > 0 the submeshes that carry no levels of their own (their sources had none) get simplified ones
// (mesh_build.h generate_lods), so a far proxy draws fewer triangles; the hlod_bake tool writes new sidecars with 2.

#include <cmath>
#include <cstring>

#include "importer_common.h"
#include "mesh_build.h"
#include "mce/cook/hlod.h"
#include "mce/asset/formats.h"
#include "mce/hash.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.hlod";
constexpr char kHlodMagic[4] = {'M', 'C', 'H', 'L'};
constexpr u32 kHlodVersion = 1;
constexpr usize kHeader = 8;   // magic, version
constexpr usize kTrailer = 8;  // FNV-1a 64 of every byte before it

// The payload between the header and the trailer, or an empty span with an error in `diags`.
Span<const byte> hlod_payload(const ImportInput& in, DiagList& diags) {
    const Span<const byte> src = in.source;
    if (src.size() < kHeader + kTrailer || std::memcmp(src.data(), kHlodMagic, sizeof(kHlodMagic)) != 0) {
        diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, "not a .mchlod file (bad magic)");
        return {};
    }
    u32 version = 0;
    std::memcpy(&version, src.data() + 4, 4);
    if (version != kHlodVersion) {
        diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, "unknown .mchlod version (re-bake the HLOD)");
        return {};
    }
    u64 stored = 0;
    std::memcpy(&stored, src.data() + src.size() - kTrailer, kTrailer);
    if (stored != fnv1a64(src.data(), src.size() - kTrailer)) {
        diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, "damaged .mchlod file (the hash does not match; re-bake the HLOD)");
        return {};
    }
    return src.subspan(kHeader, src.size() - kHeader - kTrailer);
}

// The baked mesh as the model importers' geometry (its quantized vertices and index ranges as they are), simplified
// levels added to the submeshes without any, written again as a CHNK_MESH.
void write_with_lods(const asset::MeshView& v, u32 levels, f32 ratio, f32 max_error, const Guid& guid, asset::ContainerWriter& out) {
    detail::Geometry geo;
    for (const asset::MeshVertex& x : v.vertices) geo.vertices.push_back(x);
    geo.skin_vertices.resize(v.vertices.size());
    for (u32 i = 0; i < v.header->index_count; ++i) geo.indices.push_back(v.index(i));
    for (u32 s = 0; s < v.submeshes.size(); ++s) {
        geo.submeshes.push_back(v.submeshes[s]);
        geo.submesh_ext.push_back(v.material_ext(s));
        geo.submesh_skin.push_back(-1);
        Array<asset::MeshLod> own;
        for (const asset::MeshLod& l : v.submesh_lods(s)) own.push_back(l);
        geo.lods.push_back(static_cast<Array<asset::MeshLod>&&>(own));
    }
    detail::generate_lods(geo, levels, ratio, max_error);
    detail::SkinTables skins;
    skins.submesh_skins.resize(geo.submeshes.size(), -1);
    detail::MeshTables tables;
    tables.nodes = v.nodes;
    tables.strings = v.strings;
    detail::write_mesh_chunk(geo, skins, tables, false, guid, out);
}

class HlodImporter final : public IImporter {
public:
    StringView id() const override { return "hlod"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Mesh; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".mchlod"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        DiagList quiet;  // cook() reports a bad file; the scan only lists what a good one names
        const Span<const byte> payload = hlod_payload(in, quiet);
        if (payload.empty()) return;
        const Result<asset::MeshView> view = asset::MeshView::read(payload);
        if (!view) return;
        (void)diags;
        for (u32 s = 0; s < view.value().submeshes.size(); ++s)
            for (u32 t = 0; t < static_cast<u32>(asset::MaterialSlot::Count); ++t) {
                const Guid g = view.value().submeshes[s].material.texture(static_cast<asset::MaterialSlot>(t));
                if (g.is_null()) continue;
                bool seen = false;
                for (const DepRef& d : out) seen |= d.guid == g;
                if (seen) continue;
                DepRef d;
                d.guid = g;
                d.expected = ChunkType::Texture;
                d.link = true;
                d.where = "submesh texture";
                out.push_back(d);
            }
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const StringView file = in.display_path;
        if (!detail::check_keys(in.settings, {"lod_levels", "lod_ratio", "lod_error"}, file, kStage, diags)) return false;
        f64 lod_levels = 0.0, lod_ratio = 0.35, lod_error = 0.05;
        if (!detail::read_number(in.settings, "lod_levels", 0.0, lod_levels, file, kStage, diags)) return false;
        if (!detail::read_number(in.settings, "lod_ratio", 0.35, lod_ratio, file, kStage, diags)) return false;
        if (!detail::read_number(in.settings, "lod_error", 0.05, lod_error, file, kStage, diags)) return false;
        if (lod_levels < 0.0 || lod_levels >= asset::kMaxMeshLods || lod_levels != std::floor(lod_levels)) {
            diags.error(Errc::InvalidArgument, file, 0, 0, kStage, "'lod_levels' must be a whole number 0..3");
            return false;
        }
        if (!(lod_ratio > 0.0 && lod_ratio < 1.0) || !(lod_error > 0.0 && lod_error <= 1.0)) {
            diags.error(Errc::InvalidArgument, file, 0, 0, kStage, "'lod_ratio' must be in (0, 1) and 'lod_error' in (0, 1]");
            return false;
        }
        const Span<const byte> payload = hlod_payload(in, diags);
        if (payload.empty()) return false;
        const Result<asset::MeshView> view = asset::MeshView::read(payload);
        if (!view) {
            diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, "the .mchlod mesh does not validate (re-bake the HLOD)");
            return false;
        }
        if (lod_levels > 0.0 && !view.value().skinned()) {
            write_with_lods(view.value(), static_cast<u32>(lod_levels), static_cast<f32>(lod_ratio), static_cast<f32>(lod_error), in.guid, out);
            return true;
        }
        const u32 flags = view.value().header->flags;
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Mesh;
        d.version = asset::chunk_version(asset::MESH_VERSION, (flags & asset::MeshFlags::Materials) ? asset::MESH_MINOR_MATERIALS
                                                              : (flags & asset::MeshFlags::Ragdoll) ? asset::MESH_MINOR_RAGDOLL
                                                              : (flags & asset::MeshFlags::Lods)    ? asset::MESH_MINOR_LODS
                                                              : (flags & asset::MeshFlags::Skinned) ? asset::MESH_MINOR_SKIN
                                                                                                    : u8{0});
        d.guid = in.guid;
        out.add(d, payload);
        return true;
    }
};

}  // namespace

void write_hlod_file(Span<const byte> mesh_payload, Array<byte>& out) {
    out.clear();
    out.resize(kHeader + mesh_payload.size() + kTrailer);
    std::memcpy(out.data(), kHlodMagic, sizeof(kHlodMagic));
    std::memcpy(out.data() + 4, &kHlodVersion, 4);
    if (!mesh_payload.empty()) std::memcpy(out.data() + kHeader, mesh_payload.data(), mesh_payload.size());
    const u64 h = fnv1a64(out.data(), out.size() - kTrailer);
    std::memcpy(out.data() + out.size() - kTrailer, &h, kTrailer);
}

mem::UniquePtr<IImporter> make_hlod_importer() { return mem::make_unique<HlodImporter>(mem::heap()); }

}  // namespace mce::cook
