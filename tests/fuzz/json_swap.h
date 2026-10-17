#pragma once
// A text mutant that stays valid JSON: one value (string, number, literal, array or object) is replaced by another kind
// of value, so a parser's own checks of types, ranges and references are reached (M-FUZZ).

#include <cstring>
#include <string>

#include "mce/containers/array.h"
#include "mce/types.h"
#include "mutator.h"

namespace mce::fuzz {

// A text mutant that stays valid JSON: one value (string, number, literal, array or object) is replaced by another
// kind of value, so the importer's own checks of types, ranges and references are reached.
inline std::string swap_value(Mutator& m, Span<const byte> src) {
    static const char* kReplacements[] = {"0",    "-1",    "1",        "2147483648", "4294967296", "1e308", "-1e308", "0.5",
                                          "\"\"", "\"x\"", "\"00000000-0000-0000-0000-000000000000\"", "true", "false", "null",
                                          "[]",   "{}",    "[0]",      "[1,2,3]",    "{\"a\":1}"};
    std::string t(reinterpret_cast<const char*>(src.data()), src.size());
    if (t.empty()) return t;
    for (int attempt = 0; attempt < 32; ++attempt) {
        usize p = static_cast<usize>(m.below(t.size()));
        const char c = t[p];
        usize e = p;
        if (c == '"') {
            // Only values, not keys: a key is followed by ':'.
            e = p + 1;
            while (e < t.size() && t[e] != '"') e += t[e] == '\\' ? 2 : 1;
            ++e;
            usize q = e;
            while (q < t.size() && (t[q] == ' ' || t[q] == '\n' || t[q] == '\t' || t[q] == '\r')) ++q;
            if (q < t.size() && t[q] == ':') continue;
        } else if ((c >= '0' && c <= '9') || c == '-') {
            while (e < t.size() && (std::strchr("0123456789.eE+-", t[e]) != nullptr)) ++e;
        } else if (c == 't' || c == 'f' || c == 'n') {
            while (e < t.size() && t[e] >= 'a' && t[e] <= 'z') ++e;
        } else if (c == '[' || c == '{') {
            int depth = 0;
            bool in_str = false;
            for (e = p; e < t.size(); ++e) {
                if (in_str) {
                    if (t[e] == '\\') ++e;
                    else if (t[e] == '"') in_str = false;
                } else if (t[e] == '"') in_str = true;
                else if (t[e] == '[' || t[e] == '{') ++depth;
                else if ((t[e] == ']' || t[e] == '}') && --depth == 0) { ++e; break; }
            }
        } else {
            continue;
        }
        if (e > t.size()) e = t.size();
        const char* r = kReplacements[m.below(sizeof(kReplacements) / sizeof(kReplacements[0]))];
        return t.substr(0, p) + r + t.substr(e);
    }
    return t;
}


}  // namespace mce::fuzz
