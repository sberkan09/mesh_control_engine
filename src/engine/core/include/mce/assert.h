#pragma once
// Programmer-error handling (core.md §2.4). Expected failures use Result<T> instead.
//
//   MCE_ASSERT(cond)        debug (or MCE_FORCE_ASSERTS): report + break/abort; release: compiled out
//   MCE_ASSERT_MSG(cond, m) same, with a static message
//   MCE_VERIFY(expr)        always evaluated; reports only when asserts are enabled
//   MCE_UNREACHABLE()       asserts, then tells the optimizer the path cannot happen

#include "mce/types.h"

#if !defined(NDEBUG) || defined(MCE_FORCE_ASSERTS)
#define MCE_ASSERTS_ENABLED 1
#else
#define MCE_ASSERTS_ENABLED 0
#endif

#if defined(_MSC_VER)
#define MCE_DEBUG_BREAK() __debugbreak()
#else
#define MCE_DEBUG_BREAK() __builtin_trap()
#endif

namespace mce {

// Returns true when the failure should break into the debugger / terminate.
// The default handler logs, flushes the breadcrumb ring and returns true.
// Tests install a handler that records the failure and returns false.
using AssertHandler = bool (*)(const char* expr, const char* file, int line, const char* msg);

AssertHandler set_assert_handler(AssertHandler handler);

namespace detail {
bool report_assert(const char* expr, const char* file, int line, const char* msg);
}

}  // namespace mce

#if MCE_ASSERTS_ENABLED
#define MCE_ASSERT_MSG(cond, msg)                                                  \
    do {                                                                           \
        if (!(cond)) [[unlikely]] {                                                \
            if (::mce::detail::report_assert(#cond, __FILE__, __LINE__, (msg))) {  \
                MCE_DEBUG_BREAK();                                                 \
            }                                                                      \
        }                                                                          \
    } while (0)
#define MCE_VERIFY(expr) MCE_ASSERT_MSG(expr, nullptr)
#else
#define MCE_ASSERT_MSG(cond, msg) \
    do {                          \
        (void)sizeof(!(cond));    \
    } while (0)
#define MCE_VERIFY(expr) ((void)(expr))
#endif

#define MCE_ASSERT(cond) MCE_ASSERT_MSG(cond, nullptr)

#if defined(_MSC_VER)
#define MCE_UNREACHABLE()                         \
    do {                                          \
        MCE_ASSERT_MSG(false, "unreachable");     \
        __assume(0);                              \
    } while (0)
#else
#define MCE_UNREACHABLE()                         \
    do {                                          \
        MCE_ASSERT_MSG(false, "unreachable");     \
        __builtin_unreachable();                  \
    } while (0)
#endif
