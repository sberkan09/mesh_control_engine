#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "mce/assert.h"
#include "mce/log.h"
#include "test_support.h"

namespace mce::test {
namespace {
u32 g_asserts = 0;
u32 g_expect_depth = 0;
const char* g_last_message = "";

bool test_assert_handler(const char* expr, const char* file, int line, const char* msg) {
    ++g_asserts;
    g_last_message = msg ? msg : expr;
    if (g_expect_depth == 0) {
        ADD_FAIL_AT(file, line, "unexpected MCE_ASSERT: " << expr << (msg ? " — " : "") << (msg ? msg : ""));
    }
    return false;  // keep running
}
}  // namespace

u32 assert_count() { return g_asserts; }
void reset_asserts() { g_asserts = 0; }
const char* last_assert_message() { return g_last_message; }

ExpectAssert::ExpectAssert() : before_(g_asserts) { ++g_expect_depth; }
ExpectAssert::~ExpectAssert() {
    --g_expect_depth;
#if MCE_ASSERTS_ENABLED
    CHECK_MESSAGE(g_asserts > before_, "expected an MCE_ASSERT to fire");
#endif
}

}  // namespace mce::test

int main(int argc, char** argv) {
    mce::log::LogDesc log_desc;
    log_desc.app_name = "test";
    log_desc.level = mce::log::Level::Warn;
    mce::log::init(log_desc);
    mce::set_assert_handler(&mce::test::test_assert_handler);

    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    const int rc = ctx.run();
    mce::log::shutdown();
    return rc;
}
