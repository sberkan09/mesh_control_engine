// M-SOURCEEDIT 1: the source document behind the Source panel. A check cooks the text in memory and reports the
// cook's line and column, a document the cook refuses is not written, a good one is, and Revert drops the edits.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/fs.h"
#include "mce/editor/asset_ops.h"
#include "mce/editor/source_doc.h"

using namespace mce;
using namespace mce::editor;

namespace {

struct Fixture {
    std::string root;
    EditorProject project;
    asset::Guid guid;
    SourceDoc doc;

    explicit Fixture(const char* name) {
        root = cook_test::fresh_dir(name);
        std::filesystem::create_directories(root + "/assets");
        REQUIRE(project.open(root));
        cook::String err;
        REQUIRE(create_asset(project, "settings/defaults.mcdefaults", guid, err));
        const ProjectAsset* a = project.find(guid);
        REQUIRE(a != nullptr);
        REQUIRE(doc.open(project, *a, err));
    }
    std::string file() {
        cook::String t;
        REQUIRE(cook::fs::read_text(project.asset_path("settings/defaults.mcdefaults").view(), t));
        return std::string(t.c_str());
    }
};

}  // namespace

TEST_CASE("SourceDoc: a fresh document is clean and checks") {
    Fixture f("source_doc_open");
    CHECK_FALSE(f.doc.dirty());
    CHECK(f.doc.path() == "settings/defaults.mcdefaults");
    CHECK(f.doc.check(f.project));
    CHECK(f.doc.problems().size() == 0);
    CHECK(f.doc.checked());
}

TEST_CASE("SourceDoc: broken JSON is a problem with a line, and nothing is written") {
    Fixture f("source_doc_broken");
    const std::string before = f.file();
    const std::string guid = asset::to_string(f.guid).c_str();
    const std::string broken = "{\n  \"guid\": \"" + guid + "\",\n  \"format\": \"mce-defaults\",\n  oops\n}\n";
    f.doc.set_text(broken.c_str());
    CHECK(f.doc.dirty());
    CHECK_FALSE(f.doc.checked());
    CHECK_FALSE(f.doc.check(f.project));
    REQUIRE(f.doc.problems().size() >= 1);
    CHECK(f.doc.problems()[0].line == 4);
    cook::String err;
    CHECK_FALSE(f.doc.save(f.project, err));
    CHECK_FALSE(err.empty());
    CHECK(f.file() == before);
    CHECK(f.doc.dirty());
}

TEST_CASE("SourceDoc: a document the cook accepts is saved, revert drops the edits") {
    Fixture f("source_doc_save");
    const std::string good = std::string(f.doc.text().data(), f.doc.text().size());
    const std::string guid = asset::to_string(f.guid).c_str();
    const std::string edited = "{\"format\":\"mce-defaults\",\"version\":1,\"guid\":\"" + guid + "\",\"convars\":{\"ui.reference_height\":720}}\n";
    f.doc.set_text(edited.c_str());
    CHECK(f.doc.check(f.project));
    cook::String err;
    REQUIRE(f.doc.save(f.project, err));
    CHECK_FALSE(f.doc.dirty());
    CHECK(f.file() == edited);

    f.doc.set_text("not json at all");
    CHECK(f.doc.dirty());
    REQUIRE(f.doc.revert(f.project, err));
    CHECK_FALSE(f.doc.dirty());
    CHECK(std::string(f.doc.text().data(), f.doc.text().size()) == edited);
    CHECK(good != edited);
}

TEST_CASE("SourceDoc: an edit that drops the GUID is refused") {
    Fixture f("source_doc_guid");
    const std::string before = f.file();
    f.doc.set_text("{\"format\":\"mce-defaults\",\"version\":1,\"convars\":{}}\n");
    CHECK_FALSE(f.doc.check(f.project));
    REQUIRE(f.doc.problems().size() == 1);
    CHECK(f.doc.problems()[0].message.view().find("GUID") != StringView::npos);
    cook::String err;
    CHECK_FALSE(f.doc.save(f.project, err));
    CHECK(f.file() == before);
    CHECK(f.project.find(f.guid) != nullptr);
}

TEST_CASE("SourceDoc: an asset that left the project reports it") {
    Fixture f("source_doc_gone");
    cook::String err;
    REQUIRE(delete_asset(f.project, f.guid, err));
    CHECK(f.doc.path() == "settings/defaults.mcdefaults");
    CHECK_FALSE(f.doc.check(f.project));
    REQUIRE(f.doc.problems().size() == 1);
    CHECK_FALSE(f.doc.save(f.project, err));
}
