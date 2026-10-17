// Cross-architecture encoder byte-equality (M-CONTENT 1b-ii): the BC chains of a generated image hash to the value pinned
// from x86_64 (bc_pinned.h). Run on an arm64 host it settles whether the scalar encoder (compiled with -ffp-contract=off)
// matches x86; until then cook golden hashes are guaranteed on x86_64 only. Registered as a slow test: the integration
// run executes it, and an arm64 host runs it by hand (ctest -R test_cook_bc_arch).

#include <doctest/doctest.h>

#include "bc_pinned.h"

TEST_CASE("bc encoder: the chains of the generated image hash to the pinned x86_64 value") {
    CHECK(bc_pinned::chain_hash() == bc_pinned::kPinnedBcHash);
}
