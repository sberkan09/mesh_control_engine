// SHA-1 and base64, only for the WebSocket handshake's Sec-WebSocket-Accept (RFC 6455 §4.2.2). SHA-1
// is not used for anything that needs it to be secure.

#include <cstring>

#include "mce/net/websocket.h"
#include "ws_crypto.h"

namespace mce::net {
namespace {

u32 rotl(u32 v, u32 n) { return (v << n) | (v >> (32 - n)); }

void sha1_block(u32 h[5], const u8* block) {
    u32 w[80];
    for (u32 i = 0; i < 16; ++i)
        w[i] = (u32(block[i * 4]) << 24) | (u32(block[i * 4 + 1]) << 16) | (u32(block[i * 4 + 2]) << 8) | block[i * 4 + 3];
    for (u32 i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (u32 i = 0; i < 80; ++i) {
        u32 f = 0, k = 0;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const u32 t = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

// SHA-1 of a message of at most 119 bytes (the handshake's is 60).
void sha1_short(const u8* data, usize size, u8 out[20]) {
    u32 h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    u8 buf[128] = {};
    std::memcpy(buf, data, size);
    buf[size] = 0x80;
    const usize blocks = size + 9 <= 64 ? 1 : 2;
    const u64 bits = u64(size) * 8;
    for (u32 i = 0; i < 8; ++i) buf[blocks * 64 - 1 - i] = static_cast<u8>(bits >> (8 * i));
    for (usize b = 0; b < blocks; ++b) sha1_block(h, buf + b * 64);
    for (u32 i = 0; i < 5; ++i)
        for (u32 j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<u8>(h[i] >> (24 - 8 * j));
}

}  // namespace

FixedString<32> websocket_accept_key(StringView key) {
    static constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    u8 input[119];
    const usize guid = sizeof(kGuid) - 1;
    const usize n = key.size() < sizeof(input) - guid ? key.size() : sizeof(input) - guid;
    std::memcpy(input, key.data(), n);
    std::memcpy(input + n, kGuid, guid);
    u8 digest[20];
    sha1_short(input, n + guid, digest);
    return FixedString<32>(detail::base64(digest, sizeof(digest)).view());
}

namespace detail {

FixedString<64> base64(const u8* data, usize size) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char text[64];
    usize o = 0;
    if (size > 45) size = 45;
    for (usize i = 0; i < size; i += 3) {
        const u32 v = (u32(data[i]) << 16) | (i + 1 < size ? u32(data[i + 1]) << 8 : 0u) | (i + 2 < size ? u32(data[i + 2]) : 0u);
        text[o++] = kAlphabet[(v >> 18) & 63];
        text[o++] = kAlphabet[(v >> 12) & 63];
        text[o++] = i + 1 < size ? kAlphabet[(v >> 6) & 63] : '=';
        text[o++] = i + 2 < size ? kAlphabet[v & 63] : '=';
    }
    return FixedString<64>(StringView(text, o));
}

}  // namespace detail
}  // namespace mce::net
