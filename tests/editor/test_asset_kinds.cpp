// M-ASSETNEW 1: the shared asset kind table. Lookups by extension and by name find the same row, an unknown
// extension finds none, and every row's template is a JSON object (the MCP tests cook each one).

#include <doctest/doctest.h>

#include "mce/cook/json.h"
#include "mce/editor/asset_kinds.h"

using namespace mce;
using namespace mce::editor;

TEST_CASE("AssetKinds: lookups agree and every template is a JSON object") {
    REQUIRE(asset_kinds().size() > 10);
    const AssetKind* seq = asset_kind_for_extension("effects/intro.mcseq");
    REQUIRE(seq);
    CHECK(StringView(seq->type) == "Sequence");
    CHECK(asset_kind_named("Sequence") == seq);
    CHECK(asset_kind_named(".mcseq") == seq);
    CHECK(asset_kind_named("mcseq") == seq);
    CHECK(asset_kind_for_extension("textures/a.png") == nullptr);
    CHECK(asset_kind_for_extension("noextension") == nullptr);

    for (const AssetKind& k : asset_kinds()) {
        cook::JsonDoc doc;
        cook::JsonError err;
        REQUIRE_MESSAGE(doc.parse(k.tmpl, err), k.type);
        CHECK_MESSAGE(doc.root().is_object(), k.type);
    }
}
