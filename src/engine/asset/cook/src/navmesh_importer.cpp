// .mcnav -> CHNK_NAV: a baked 3D navmesh (physics::bake_navmesh, M-GAMEPLAY3D 1), stored as baked.
// The editor and MCP's navmesh_bake write the file; the runtime loads the payload with
// physics::NavMesh3D::load, which validates it in full. Here only the magic and version are checked
// (the cook does not link the navigation code).

#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.navmesh";
constexpr char kNavMagic[4] = {'M', 'C', 'N', 'V'};
constexpr u32 kNavBlobVersion = 1;

class NavMeshImporter final : public IImporter {
public:
    StringView id() const override { return "navmesh"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::NavMesh; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".mcnav"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        if (!detail::check_keys(in.settings, {}, in.display_path, kStage, diags)) return false;
        const Span<const byte> src = in.source;
        u32 version = 0;
        if (src.size() < 8 || std::memcmp(src.data(), kNavMagic, sizeof(kNavMagic)) != 0) {
            diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, "not a .mcnav file (bad magic)");
            return false;
        }
        std::memcpy(&version, src.data() + 4, 4);
        if (version != kNavBlobVersion) {
            diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, "unknown .mcnav version (re-bake the navmesh)");
            return false;
        }
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::NavMesh;
        d.version = asset::chunk_version(asset::NAVMESH_VERSION, 0);
        d.guid = in.guid;
        out.add(d, src);
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_navmesh_importer() { return mem::make_unique<NavMeshImporter>(mem::heap()); }

}  // namespace mce::cook
