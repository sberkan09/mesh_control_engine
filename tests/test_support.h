#pragma once
// Test helpers: the test main installs an assert handler that records failures instead of
// aborting, so tests can assert that a programmer-error path fires.

#include "mce/types.h"

namespace mce::test {

// Number of MCE_ASSERT failures since the last reset.
u32 assert_count();
void reset_asserts();
// Message of the most recent assert failure ("" if none).
const char* last_assert_message();

// Scope that expects at least one assert failure inside it; fails the test otherwise.
class ExpectAssert {
public:
    ExpectAssert();
    ~ExpectAssert();

private:
    [[maybe_unused]] u32 before_;
};

}  // namespace mce::test
