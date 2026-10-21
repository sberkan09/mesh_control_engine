#pragma once
// String: growable, NUL-terminated char buffer for cook-side code (paths, diagnostics, JSON
// output). Engine targets don't use STL strings (CI container wall); the cook is engine code too,
// since the editor links it.

#include <cstring>

#include "mce/containers/array.h"

namespace mce::cook {

class String {
public:
    String() { buf_.push_back('\0'); }
    String(StringView s) : String() { append(s); }  // NOLINT(google-explicit-constructor)
    String(const char* s) : String(StringView(s)) {}  // NOLINT(google-explicit-constructor)

    String& append(StringView s) {
        buf_.pop_back();
        for (char c : s) buf_.push_back(c);
        buf_.push_back('\0');
        return *this;
    }
    String& append(char c) {
        buf_.back() = c;
        buf_.push_back('\0');
        return *this;
    }
    String& assign(StringView s) {
        clear();
        return append(s);
    }
    String& operator+=(StringView s) { return append(s); }
    String& operator+=(char c) { return append(c); }
    void clear() {
        buf_.clear();
        buf_.push_back('\0');
    }

    usize size() const { return buf_.size() - 1; }
    bool empty() const { return size() == 0; }
    const char* c_str() const { return buf_.data(); }
    char* data() { return buf_.data(); }
    StringView view() const { return StringView(buf_.data(), size()); }
    operator StringView() const { return view(); }  // NOLINT(google-explicit-constructor)

    friend bool operator==(const String& a, const String& b) { return a.view() == b.view(); }
    friend bool operator==(const String& a, StringView b) { return a.view() == b; }
    friend bool operator<(const String& a, const String& b) { return a.view() < b.view(); }

private:
    Array<char> buf_;
};

// printf-style append (the cook's diagnostic formatting).
void appendf(String& out, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// "a" + "/" + "b" with exactly one separator.
String path_join(StringView a, StringView b);
// Extension including the dot, lower-cased (".png"); empty if none.
String lower_extension(StringView path);

}  // namespace mce::cook
