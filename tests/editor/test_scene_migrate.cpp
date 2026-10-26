// M-SCENEMIGRATE: a renamed component keeps old scenes loading.

#include <doctest/doctest.h>

#include <string_view>

#include "mce/ecs/transform.h"
#include "mce/editor/scene_document.h"

using namespace mce;
using namespace mce::editor;

namespace {

constexpr const char* kOld =
    "{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"5ce2e000-0000-4000-8000-000000000010\", \"name\": \"old\",\n"
    "  \"entities\": [ { \"name\": \"Crate\", \"components\": { \"OldTransform\": { \"position\": [1, 2, 3] } } } ] }\n";

constexpr ComponentRename kTable[] = {{1, "OldTransform", "LocalTransform"}};

}  // namespace

TEST_CASE("a component under its old name is refused without a rename row") {
    SceneDocument doc;
    cook::DiagList diags;
    CHECK_FALSE(doc.parse(kOld, "old.mcscene", diags));
}

TEST_CASE("a rename row loads the old spelling as the new component, and a save writes the new one") {
    SceneDocument doc;
    doc.set_renames(kTable, 1);
    cook::DiagList diags;
    REQUIRE(doc.parse(kOld, "old.mcscene", diags));
    REQUIRE(doc.size() == 1);
    CHECK(doc.at(0).find(refl::type_of<LocalTransform>()) != nullptr);

    cook::String out;
    doc.serialize(out);
    const std::string_view text(out.c_str());
    CHECK(text.find("LocalTransform") != std::string_view::npos);
    CHECK(text.find("OldTransform") == std::string_view::npos);
}

TEST_CASE("a rename row does not apply to a file newer than its up_to_version") {
    constexpr ComponentRename table[] = {{0, "OldTransform", "LocalTransform"}};
    SceneDocument doc;
    doc.set_renames(table, 1);
    cook::DiagList diags;
    CHECK_FALSE(doc.parse(kOld, "old.mcscene", diags));
}
