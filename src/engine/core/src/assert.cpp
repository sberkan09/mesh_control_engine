#include "mce/assert.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "mce/log.h"

namespace mce {
namespace {

void print_breadcrumb(const log::ConsoleLine& line, void*) {
    std::fprintf(stderr, "  crumb #%llu: %s\n", static_cast<unsigned long long>(line.seq), line.text.c_str());
}

bool default_handler(const char* expr, const char* file, int line, const char* msg) {
    std::fprintf(stderr, "MCE_ASSERT failed: %s\n  at %s:%d\n  %s\n", expr, file, line, msg ? msg : "");
    MCE_LOG_CRITICAL("assert failed: {} ({}:{}) {}", expr, file, line, msg ? msg : "");
    std::fprintf(stderr, "breadcrumbs (oldest first):\n");
    log::visit_breadcrumbs(&print_breadcrumb, nullptr);
    std::fflush(stderr);
    return true;
}

std::atomic<AssertHandler> g_handler{&default_handler};

}  // namespace

AssertHandler set_assert_handler(AssertHandler handler) {
    return g_handler.exchange(handler ? handler : &default_handler);
}

namespace detail {
bool report_assert(const char* expr, const char* file, int line, const char* msg) {
    return g_handler.load()(expr, file, line, msg);
}
}  // namespace detail

}  // namespace mce
