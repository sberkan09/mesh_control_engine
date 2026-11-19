// M-BATCHRENAME 1: names for a whole selection.

#include <doctest/doctest.h>

#include <random>
#include <string>

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

// Found by the fuzz below: a run of more than 16 '#' was padded to 16 digits, not to the run's length.
TEST_CASE("batch_rename_expand pads a long run to its length") {
    CHECK(batch_rename_expand("##################", "Old", 6, 1).view() == "000000000000000007");  // 18 wide
}

// M-BATCHRENAME2: random patterns against a plain expansion.
TEST_CASE("batch_rename_expand fuzz: equals a plain expansion of the pattern") {
    static const char* const kOld[] = {"", "Old", "Guard 7", "0123456789012345678901234567890123456789"};
    u32 wide = 0;
    for (u32 seed = 1; seed <= 500; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        std::string pattern;
        for (u32 i = 0, n = static_cast<u32>(rng() % 8); i < n; ++i) {
            switch (rng() % 4) {
                case 0:
                    pattern += '*';
                    break;
                case 1:
                    pattern += std::string(1 + rng() % 20, '#');
                    break;
                case 2:
                    pattern += "Ab_";
                    break;
                default:
                    pattern += static_cast<char>('c' + rng() % 3);
                    break;
            }
        }
        const std::string old = kOld[rng() % 4];
        const u32 index = static_cast<u32>(rng() % 1000);
        const u32 start = static_cast<u32>(rng() % 3000);
        std::string want;
        for (usize i = 0; i < pattern.size();) {
            if (pattern[i] == '*') {
                want += old;
                ++i;
            } else if (pattern[i] == '#') {
                usize width = 0;
                while (i < pattern.size() && pattern[i] == '#') ++width, ++i;
                wide += width > 16 ? 1 : 0;
                std::string num = std::to_string(start + index);
                if (num.size() < width) num.insert(0, width - num.size(), '0');
                want += num;
            } else {
                want += pattern[i++];
            }
        }
        if (want.size() > 63) want.resize(63);
        const FixedString<64> got = batch_rename_expand(StringView(pattern.data(), pattern.size()),
                                                        StringView(old.data(), old.size()), index, start);
        CHECK(std::string(got.view().data(), got.view().size()) == want);
    }
    CHECK(wide > 5);
}
