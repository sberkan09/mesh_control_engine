// M-ASSETREFS 1: who names whom. An asset that writes another's GUID is its user (once, however often it writes it),
// a self mention does not count, and an asset nobody names is listed as unused.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/fs.h"
#include "mce/editor/asset_ops.h"
#include "mce/editor/asset_refs.h"

using namespace mce;
using namespace mce::editor;

TEST_CASE("AssetRefs: users, use counts and the unused list") {
    const std::string root = cook_test::fresh_dir("asset_refs");
    std::filesystem::create_directories(root + "/assets");
    EditorProject project;
    REQUIRE(project.open(root));
    asset::Guid a, b, c;
    cook::String err;
    REQUIRE(create_asset(project, "fx/a.mcseq", a, err));
    REQUIRE(create_asset(project, "fx/b.mcseq", b, err));
    REQUIRE(create_asset(project, "fx/c.mcseq", c, err));

    // b names a twice (and itself once, which does not count).
    cook::String text;
    REQUIRE(cook::fs::read_text(project.asset_path("fx/b.mcseq").view(), text));
    std::string edited(text.c_str());
    edited += "\n// guid:" + std::string(asset::to_string(a).c_str()) + " and again " + std::string(asset::to_string(a).c_str()) + "\n";
    std::ofstream(root + "/assets/fx/b.mcseq", std::ios::binary) << edited;
    project.rescan();

    AssetRefs refs;
    refs.build(project);
    CHECK(refs.use_count(a) == 1);
    CHECK(refs.use_count(b) == 0);
    Array<cook::String> users;
    refs.used_by(a, users);
    REQUIRE(users.size() == 1);
    CHECK(users[0].view() == "fx/b.mcseq");

    Array<asset::Guid> unused;
    refs.unused(unused);
    REQUIRE(unused.size() == 2);  // b and c; a is named
    bool has_b = false, has_c = false;
    for (const asset::Guid g : unused) {
        has_b |= g == b;
        has_c |= g == c;
    }
    CHECK(has_b);
    CHECK(has_c);
}
