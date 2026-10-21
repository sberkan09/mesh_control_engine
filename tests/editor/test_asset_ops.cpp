// M-ASSETNEW 2: creating, duplicating and deleting assets. A new file is a cooked starter of its kind with a fresh
// GUID, a path that is taken or malformed writes nothing, a copy gets its own GUID (a sidecar source its own sidecar)
// and the references to a deleted asset are listed first.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/fs.h"
#include "mce/editor/asset_ops.h"

using namespace mce;
using namespace mce::editor;

namespace {

struct Fixture {
    std::string root;
    EditorProject project;

    explicit Fixture(const char* name) {
        root = cook_test::fresh_dir(name);
        std::filesystem::create_directories(root + "/assets");
        REQUIRE(project.open(root));
    }
    bool has(const char* rel) { return cook::fs::exists(project.asset_path(rel).view()); }
};

}  // namespace

TEST_CASE("AssetOps: every kind can be created from its starter and cooks") {
    Fixture f("asset_ops_create_all");
    for (const AssetKind& k : asset_kinds()) {
        cook::String rel("new/");
        rel += k.type;
        rel += k.ext;
        asset::Guid g;
        cook::String err;
        const bool ok = create_asset(f.project, rel.view(), g, err);
        INFO(std::string(err.view()).substr(0, 300));
        CHECK_MESSAGE(ok, k.type);
        if (ok) {
            CHECK(f.project.find(g) != nullptr);
            CHECK(f.has(rel.c_str()));
        }
    }
}

TEST_CASE("AssetOps: a bad or taken path writes nothing") {
    Fixture f("asset_ops_create_bad");
    asset::Guid g;
    cook::String err;
    CHECK_FALSE(create_asset(f.project, "../outside.mcseq", g, err));
    CHECK_FALSE(create_asset(f.project, "dir//x.mcseq", g, err));
    CHECK_FALSE(create_asset(f.project, "x.unknownext", g, err));
    CHECK_FALSE(create_asset(f.project, "noext", g, err));
    CHECK(create_asset(f.project, "fx/intro.mcseq", g, err));
    err = cook::String();
    CHECK_FALSE(create_asset(f.project, "fx/intro.mcseq", g, err));
    CHECK(err.view().find("already exists") != StringView::npos);
}

TEST_CASE("AssetOps: a duplicate gets a fresh GUID; a sidecar source gets its own sidecar") {
    Fixture f("asset_ops_duplicate");
    std::ofstream(f.root + "/assets/en.csv") << "key,text\nhello,Hello\n";
    std::ofstream(f.root + "/assets/en.csv.mceasset") << R"({ "guid": "5ce4e000-0000-4000-8000-0000000000e1", "importer": "strings" })";
    f.project.rescan();

    asset::Guid seq, copy, csv_copy;
    cook::String err;
    REQUIRE(create_asset(f.project, "fx/a.mcseq", seq, err));
    REQUIRE(duplicate_asset(f.project, seq, "fx/b.mcseq", copy, err));
    CHECK_FALSE(copy == seq);
    CHECK(f.project.find(copy) != nullptr);
    CHECK(f.project.find(seq) != nullptr);
    CHECK_FALSE(duplicate_asset(f.project, seq, "fx/b.mcseq", copy, err));       // taken
    CHECK_FALSE(duplicate_asset(f.project, seq, "fx/c.mcpart", copy, err));      // another extension

    const asset::Guid csv = asset::parse_guid("5ce4e000-0000-4000-8000-0000000000e1").value();
    REQUIRE(duplicate_asset(f.project, csv, "tr.csv", csv_copy, err));
    CHECK_FALSE(csv_copy == csv);
    CHECK(f.has("tr.csv.mceasset"));
    CHECK(f.project.find(csv_copy) != nullptr);
}

TEST_CASE("AssetOps: delete removes the file and its sidecar; references are listed first") {
    Fixture f("asset_ops_delete");
    std::ofstream(f.root + "/assets/en.csv") << "key,text\nhello,Hello\n";
    std::ofstream(f.root + "/assets/en.csv.mceasset") << R"({ "guid": "5ce4e000-0000-4000-8000-0000000000e1", "importer": "strings" })";
    f.project.rescan();
    const asset::Guid csv = asset::parse_guid("5ce4e000-0000-4000-8000-0000000000e1").value();

    asset::Guid seq;
    cook::String err;
    REQUIRE(create_asset(f.project, "fx/a.mcseq", seq, err));
    // Another table whose text names the first by guid.
    std::ofstream(f.root + "/assets/fr.csv") << "key,text\nsee,5ce4e000-0000-4000-8000-0000000000e1\n";
    std::ofstream(f.root + "/assets/fr.csv.mceasset") << R"({ "guid": "5ce4e000-0000-4000-8000-0000000000e3", "importer": "strings" })";
    f.project.rescan();

    Array<cook::String> refs;
    references_to(f.project, csv, refs);
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].view() == "fr.csv");
    refs.clear();
    references_to(f.project, seq, refs);
    CHECK(refs.size() == 0);

    REQUIRE(delete_asset(f.project, csv, err));
    CHECK_FALSE(f.has("en.csv"));
    CHECK_FALSE(f.has("en.csv.mceasset"));
    CHECK(f.project.find(csv) == nullptr);
    CHECK_FALSE(delete_asset(f.project, csv, err));
}
