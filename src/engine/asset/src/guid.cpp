#include "mce/asset/guid.h"

namespace mce::asset {

namespace {
constexpr char kHex[] = "0123456789abcdef";

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void put_u64(char* out, u64 v) {
    for (int i = 0; i < 16; ++i) out[i] = kHex[(v >> (60 - 4 * i)) & 0xF];
}
}  // namespace

FixedString<40> to_hex(Guid g) {
    char buf[32];
    put_u64(buf, g.hi);
    put_u64(buf + 16, g.lo);
    return FixedString<40>(StringView(buf, 32));
}

GuidString to_string(Guid g) {
    char hex[32];
    put_u64(hex, g.hi);
    put_u64(hex + 16, g.lo);
    char buf[36];
    usize o = 0;
    for (usize i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) buf[o++] = '-';
        buf[o++] = hex[i];
    }
    return GuidString(StringView(buf, 36));
}

Result<Guid> parse_guid(StringView text) {
    if (text.size() != 36) return Error(Errc::ParseError, "asset.guid.parse", text);
    u64 parts[2] = {0, 0};
    usize nibble = 0;
    for (usize i = 0; i < 36; ++i) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return Error(Errc::ParseError, "asset.guid.parse", text, static_cast<u32>(i));
            continue;
        }
        const int v = hex_value(c);
        if (v < 0) return Error(Errc::ParseError, "asset.guid.parse", text, static_cast<u32>(i));
        u64& p = parts[nibble / 16];
        p = (p << 4) | static_cast<u64>(v);
        ++nibble;
    }
    return Guid{parts[0], parts[1]};
}

}  // namespace mce::asset
