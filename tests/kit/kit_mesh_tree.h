#pragma once
// The kit tests' cooked mesh tree: one mesh blob written as a dev cooked tree (manifest +
// content-addressed file) that an AssetSystem mounts.

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "mce/asset/asset_system.h"
#include "mce/asset/container.h"
#include "mce/asset/formats.h"
#include "mce/asset/xxhash.h"

namespace kit_test {

using namespace mce;

// A dev cooked tree holding one mesh (manifest + content-addressed file), as the asset tests write.
inline std::string write_mesh_tree(const char* name, asset::Guid guid, Span<const byte> blob) {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(MCE_TEST_TMP_DIR) / name;
    fs::remove_all(root);
    auto write = [](const fs::path& p, Span<const byte> b) {
        fs::create_directories(p.parent_path());
        std::FILE* f = std::fopen(p.string().c_str(), "wb");
        REQUIRE(f);
        std::fwrite(b.data(), 1, b.size(), f);
        std::fclose(f);
    };
    asset::ContainerWriter w;
    asset::ContainerWriter::ChunkDesc d;
    d.type = asset::ChunkType::Mesh;
    d.version = asset::chunk_version(asset::MESH_VERSION, 0);
    d.guid = guid;
    w.add(d, blob);
    Array<byte> mesh;
    w.finish(mesh);
    const u64 hash = asset::xxh64(mesh.data(), mesh.size());
    const std::string hex = asset::to_hex(guid).c_str();
    char file[64];
    std::snprintf(file, sizeof file, "%016llx.mce", static_cast<unsigned long long>(hash));
    write(root / hex.substr(0, 2) / hex / file, mesh.span());
    asset::ManifestItem item{guid, hash, asset::ChunkType::Mesh, "models/box.glb"};
    Array<byte> mp;
    asset::write_manifest(Span<asset::ManifestItem>(&item, 1), mp);
    asset::ContainerWriter mw;
    asset::ContainerWriter::ChunkDesc md;
    md.type = asset::ChunkType::Manifest;
    mw.add(md, mp.span());
    Array<byte> manifest;
    mw.finish(manifest);
    write(root / "manifest.mce", manifest.span());
    return root.generic_string();
}

}  // namespace kit_test
