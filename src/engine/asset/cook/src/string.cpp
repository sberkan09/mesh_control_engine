#include "mce/cook/string.h"

#include <cstdarg>
#include <cstdio>

namespace mce::cook {

void appendf(String& out, const char* fmt, ...) {
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    const int n = std::vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    if (static_cast<usize>(n) < sizeof(small)) {
        out.append(StringView(small, static_cast<usize>(n)));
    } else {
        Array<char> big;
        big.resize(static_cast<usize>(n) + 1);
        std::vsnprintf(big.data(), big.size(), fmt, ap2);
        out.append(StringView(big.data(), static_cast<usize>(n)));
    }
    va_end(ap2);
}

String path_join(StringView a, StringView b) {
    String s(a);
    if (!a.empty() && a.back() != '/' && a.back() != '\\') s += '/';
    while (!b.empty() && (b.front() == '/' || b.front() == '\\')) b.remove_prefix(1);
    s += b;
    return s;
}

String lower_extension(StringView path) {
    const usize slash = path.find_last_of("/\\");
    const usize dot = path.find_last_of('.');
    if (dot == StringView::npos || (slash != StringView::npos && dot < slash)) return {};
    String s;
    for (char c : path.substr(dot)) s += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    return s;
}

}  // namespace mce::cook
