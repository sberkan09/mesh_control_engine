// M-THUMBS 3: the thumbnail cache, headless. The render budget spreads a folder over frames, a cached picture is
// not drawn again, an edited source is (after the recheck interval, the old picture meanwhile), a new size drops
// everything, entries of removed assets go, and the cache stays under its cap.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/fs.h"
#include "mce/cook/image.h"
#include "mce/editor/asset_thumb.h"
#include "mce/editor/project.h"
#include "mce/editor/thumb_cache.h"

using namespace mce;
using namespace mce::editor;

namespace {

std::string thumb_project(const char* name) {
    const std::string root = cook_test::fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    return root;
}

const ProjectAsset* first_of(const EditorProject& p, asset::ChunkType type, usize skip = 0) {
    for (const ProjectAsset& a : p.assets())
        if (a.type == type && skip-- == 0) return &a;
    return nullptr;
}

}  // namespace

TEST_CASE("ThumbCache: budget per frame, cached pictures, stale ones redrawn, size change, removed assets") {
    const std::string root = thumb_project("thumb_cache");
    EditorProject project;
    REQUIRE(project.open(root));
    const ProjectAsset* a = first_of(project, asset::ChunkType::Texture, 0);
    const ProjectAsset* b = first_of(project, asset::ChunkType::Texture, 1);
    REQUIRE(a);
    REQUIRE(b);

    ThumbCache cache;
    cache.begin_frame(1);  // one render per frame
    const ThumbCache::Entry* ea = cache.get(project, *a, 32);
    REQUIRE(ea);
    CHECK(ea->image.width == 32);
    CHECK(ea->real);
    CHECK(ea->revision == 1);
    CHECK(cache.get(project, *b, 32) == nullptr);  // out of budget this frame
    CHECK(cache.renders() == 1);
    cache.begin_frame(1);
    CHECK(cache.get(project, *b, 32) != nullptr);
    CHECK(cache.get(project, *a, 32)->revision == 1);  // cached, not drawn again
    CHECK(cache.renders() == 2);

    // Edit a's source: the old picture stays until the recheck, then one redraw.
    const cook::String path = project.asset_path(a->path.view());
    std::filesystem::last_write_time(std::string(path.c_str()), std::filesystem::last_write_time(std::string(path.c_str())) + std::chrono::seconds(7));
    for (u32 f = 0; f < 10; ++f) cache.begin_frame(4);
    CHECK(cache.get(project, *a, 32)->revision == 1);  // not looked at yet
    for (u32 f = 0; f < ThumbCache::kRecheckFrames; ++f) cache.begin_frame(4);
    CHECK(cache.get(project, *a, 32)->revision == 2);
    CHECK(cache.get(project, *a, 32)->revision == 2);

    // A new size drops every picture.
    cache.begin_frame(4);
    const ThumbCache::Entry* big = cache.get(project, *a, 48);
    REQUIRE(big);
    CHECK(big->image.width == 48);
    CHECK(big->revision == 1);
    CHECK(cache.size() == 1);

    // A scene is a plain tile.
    const ProjectAsset* scene = first_of(project, asset::ChunkType::Scene);
    REQUIRE(scene);
    CHECK_FALSE(cache.get(project, *scene, 48)->real);
}

TEST_CASE("ThumbCache: trim keeps the cap and drops the longest unused; drop_missing lists removed assets") {
    const std::string root = thumb_project("thumb_cache_trim");
    EditorProject project;
    REQUIRE(project.open(root));
    ThumbCache cache;
    // Every catalog asset gets an entry (the cap itself is far above the sample project).
    cache.begin_frame(1000);
    for (const ProjectAsset& a : project.assets()) cache.get(project, a, 16);
    const usize n = cache.size();
    CHECK(n == project.assets().size());
    cache.trim();
    CHECK(cache.size() == n);  // under the cap: nothing dropped
    CHECK(ThumbCache::kMaxEntries >= 512);

    Array<asset::Guid> gone;
    cache.drop_missing(project, gone);
    CHECK(gone.empty());
    // Remove one source file and rescan: its entry is reported.
    const ProjectAsset* victim = first_of(project, asset::ChunkType::Texture);
    REQUIRE(victim);
    const asset::Guid vg = victim->guid;
    std::filesystem::remove(std::string(project.asset_path(victim->path.view()).c_str()));
    std::filesystem::remove(std::string(project.asset_path(victim->path.view()).c_str()) + ".mceasset");
    project.rescan();
    cache.drop_missing(project, gone);
    REQUIRE(gone.size() == 1);
    CHECK(gone[0] == vg);
    CHECK(cache.size() == n - 1);
}
