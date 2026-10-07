// Second translation unit: includes the shared types again (they must not register twice) and
// declares a type nothing ever odr-uses (its registrar must still run).

#include <doctest/doctest.h>

#include "test_types.h"

namespace refl_test {

struct OnlyHere {
    i32 a = 1;
    i32 b = 2;
};
MCE_COMPONENT(OnlyHere, a, b)

}  // namespace refl_test

namespace refl_test {
const mce::refl::TypeRecord* health_record_from_second_tu() { return &mce::refl::type_of<Health>(); }
}  // namespace refl_test
