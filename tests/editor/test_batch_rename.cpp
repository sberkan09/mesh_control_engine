// M-BATCHRENAME 1: names for a whole selection.

#include <doctest/doctest.h>

#include "mce/editor/batch_rename.h"

using namespace mce;
using namespace mce::editor;

TEST_CASE("batch_rename_expand numbers and keeps names") {
    CHECK(batch_rename_expand("Torch_##", "Old", 0, 1).view() == "Torch_01");
    CHECK(batch_rename_expand("Torch_##", "Old", 11, 1).view() == "Torch_12");
    CHECK(batch_rename_expand("Torch_##", "Old", 98, 1).view() == "Torch_99");
    CHECK(batch_rename_expand("Torch_##", "Old", 99, 1).view() == "Torch_100");  // the width is a minimum
    CHECK(batch_rename_expand("T#", "Old", 4, 0).view() == "T4");
    CHECK(batch_rename_expand("Torch", "Old", 7, 1).view() == "Torch");  // no mark: every entity gets the same name
    CHECK(batch_rename_expand("New_*", "Old", 0, 1).view() == "New_Old");
    CHECK(batch_rename_expand("*_###", "Guard", 2, 10).view() == "Guard_012");
    CHECK(batch_rename_expand("", "Old", 0, 1).view() == "");
}

TEST_CASE("batch_rename_expand cuts to the name capacity") {
    const FixedString<64> r = batch_rename_expand("*_*_*", "0123456789012345678901234567890", 0, 1);
    CHECK(r.size() == 63);
}
