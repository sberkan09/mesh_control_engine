#include "agent_image.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mce/cook/fs.h"

namespace mce::mcp {

namespace {

constexpr const char* kStage = "mcp.image";

u8 chan(u32 rgba, u32 shift) { return static_cast<u8>((rgba >> shift) & 0xFF); }

void put(cook::Image& img, i32 x, i32 y, u32 rgba, u32 alpha = 255) {
    if (x < 0 || y < 0 || static_cast<u32>(x) >= img.width || static_cast<u32>(y) >= img.height) return;
    u8* p = &img.rgba[(static_cast<usize>(y) * img.width + static_cast<usize>(x)) * 4];
    for (u32 c = 0; c < 3; ++c) {
        const u32 src = chan(rgba, 24 - c * 8);
        p[c] = static_cast<u8>((src * alpha + p[c] * (255 - alpha)) / 255);
    }
    p[3] = 255;
}

// 3x5 glyphs: five rows of three bits, top row first, high bit left.
struct Glyph {
    char c;
    u8 rows[5];
};
constexpr Glyph kGlyphs[] = {
    {'0', {7, 5, 5, 5, 7}}, {'1', {2, 6, 2, 2, 7}}, {'2', {7, 1, 7, 4, 7}}, {'3', {7, 1, 7, 1, 7}},
    {'4', {5, 5, 7, 1, 1}}, {'5', {7, 4, 7, 1, 7}}, {'6', {7, 4, 7, 5, 7}}, {'7', {7, 1, 1, 1, 1}},
    {'8', {7, 5, 7, 5, 7}}, {'9', {7, 5, 7, 1, 7}}, {'A', {2, 5, 7, 5, 5}}, {'B', {6, 5, 6, 5, 6}},
    {'C', {3, 4, 4, 4, 3}}, {'D', {6, 5, 5, 5, 6}}, {'E', {7, 4, 6, 4, 7}}, {'F', {7, 4, 6, 4, 4}},
    {'G', {3, 4, 5, 5, 3}}, {'H', {5, 5, 7, 5, 5}}, {'I', {7, 2, 2, 2, 7}}, {'J', {1, 1, 1, 5, 2}},
    {'K', {5, 5, 6, 5, 5}}, {'L', {4, 4, 4, 4, 7}}, {'M', {5, 7, 7, 5, 5}}, {'N', {6, 5, 5, 5, 5}},
    {'O', {2, 5, 5, 5, 2}}, {'P', {6, 5, 6, 4, 4}}, {'Q', {2, 5, 5, 6, 3}}, {'R', {6, 5, 6, 5, 5}},
    {'S', {3, 4, 2, 1, 6}}, {'T', {7, 2, 2, 2, 2}}, {'U', {5, 5, 5, 5, 7}}, {'V', {5, 5, 5, 5, 2}},
    {'W', {5, 5, 7, 7, 5}}, {'X', {5, 5, 2, 5, 5}}, {'Y', {5, 5, 2, 2, 2}}, {'Z', {7, 1, 2, 4, 7}},
    {'=', {0, 7, 0, 7, 0}}, {':', {0, 2, 0, 2, 0}}, {'-', {0, 0, 7, 0, 0}}, {'.', {0, 0, 0, 0, 2}},
    {'#', {5, 7, 5, 7, 5}}, {'/', {1, 1, 2, 4, 4}}, {' ', {0, 0, 0, 0, 0}},
};

const Glyph* glyph(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (const Glyph& g : kGlyphs)
        if (g.c == c) return &g;
    return nullptr;
}

}  // namespace

Result<cook::Image> read_ppm(StringView path) {
    cook::String p(path);
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return Error(Errc::NotFound, kStage, "cannot open the captured frame");
    unsigned w = 0, h = 0, maxv = 0;
    const int n = std::fscanf(f, "P6 %u %u %u", &w, &h, &maxv);
    if (n != 3 || maxv != 255 || w == 0 || h == 0 || w > 16384 || h > 16384 || std::fgetc(f) == EOF) {
        std::fclose(f);
        return Error(Errc::ParseError, kStage, "the captured frame is not a P6 PPM");
    }
    cook::Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(usize(w) * h * 4);
    Array<u8> row;
    row.resize(usize(w) * 3);
    for (u32 y = 0; y < h; ++y) {
        if (std::fread(row.data(), 1, row.size(), f) != row.size()) {
            std::fclose(f);
            return Error(Errc::ParseError, kStage, "the captured frame is truncated");
        }
        for (u32 x = 0; x < w; ++x) {
            u8* o = &img.rgba[(usize(y) * w + x) * 4];
            o[0] = row[x * 3];
            o[1] = row[x * 3 + 1];
            o[2] = row[x * 3 + 2];
            o[3] = 255;
        }
    }
    std::fclose(f);
    return img;
}

Result<cook::Image> read_png_file(StringView path) {
    cook::String p(path);
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return Error(Errc::NotFound, kStage, "cannot open the PNG");
    Array<byte> bytes;
    byte buf[65536];
    usize got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        const usize at = bytes.size();
        bytes.resize(at + got);
        std::memcpy(bytes.data() + at, buf, got);
    }
    std::fclose(f);
    return cook::decode_png(bytes.span());
}

Result<void> write_png_file(StringView path, const cook::Image& img) {
    Array<byte> png;
    MCE_TRY(cook::encode_png(img.width, img.height, img.rgba.span(), png));
    usize sep = 0;
    for (usize i = 0; i < path.size(); ++i)
        if (path[i] == '/' || path[i] == '\\') sep = i;
    if (sep > 0) MCE_TRY(cook::fs::create_directories(path.substr(0, sep)));
    return cook::fs::write_file_atomic(path, png.span());
}

void base64_append(Span<const byte> bytes, cook::String& out) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    usize i = 0;
    char quad[5] = {};
    for (; i + 2 < bytes.size(); i += 3) {
        const u32 v = (u32(u8(bytes[i])) << 16) | (u32(u8(bytes[i + 1])) << 8) | u32(u8(bytes[i + 2]));
        quad[0] = kAlphabet[(v >> 18) & 63];
        quad[1] = kAlphabet[(v >> 12) & 63];
        quad[2] = kAlphabet[(v >> 6) & 63];
        quad[3] = kAlphabet[v & 63];
        out.append(StringView(quad, 4));
    }
    if (i < bytes.size()) {
        const bool two = i + 1 < bytes.size();
        const u32 v = (u32(u8(bytes[i])) << 16) | (two ? u32(u8(bytes[i + 1])) << 8 : 0u);
        quad[0] = kAlphabet[(v >> 18) & 63];
        quad[1] = kAlphabet[(v >> 12) & 63];
        quad[2] = two ? kAlphabet[(v >> 6) & 63] : '=';
        quad[3] = '=';
        out.append(StringView(quad, 4));
    }
}

Result<void> png_base64(const cook::Image& img, cook::String& out) {
    Array<byte> png;
    MCE_TRY(cook::encode_png(img.width, img.height, img.rgba.span(), png));
    out = cook::String();
    base64_append(png.span(), out);
    return ok();
}

cook::Image make_image(u32 width, u32 height, u32 rgba) {
    cook::Image img;
    img.width = width;
    img.height = height;
    img.rgba.resize(usize(width) * height * 4);
    for (usize i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i] = chan(rgba, 24);
        img.rgba[i + 1] = chan(rgba, 16);
        img.rgba[i + 2] = chan(rgba, 8);
        img.rgba[i + 3] = chan(rgba, 0);
    }
    return img;
}

void blit(cook::Image& dst, const cook::Image& src, i32 x, i32 y) {
    for (u32 sy = 0; sy < src.height; ++sy) {
        const i64 dy = i64(y) + sy;
        if (dy < 0 || dy >= i64(dst.height)) continue;
        for (u32 sx = 0; sx < src.width; ++sx) {
            const i64 dx = i64(x) + sx;
            if (dx < 0 || dx >= i64(dst.width)) continue;
            std::memcpy(&dst.rgba[(usize(dy) * dst.width + usize(dx)) * 4], &src.rgba[(usize(sy) * src.width + sx) * 4], 4);
        }
    }
}

void fill_rect(cook::Image& img, i32 x, i32 y, i32 w, i32 h, u32 rgba) {
    for (i32 yy = y; yy < y + h; ++yy)
        for (i32 xx = x; xx < x + w; ++xx) put(img, xx, yy, rgba, chan(rgba, 0));
}

void outline_rect(cook::Image& img, i32 x, i32 y, i32 w, i32 h, u32 rgba, u32 thickness, u32 alpha) {
    const i32 t = static_cast<i32>(thickness);
    for (i32 yy = y; yy < y + h; ++yy) {
        for (i32 xx = x; xx < x + w; ++xx) {
            const bool edge = xx < x + t || xx >= x + w - t || yy < y + t || yy >= y + h - t;
            if (edge) put(img, xx, yy, rgba, alpha);
        }
    }
}

void draw_label(cook::Image& img, i32 x, i32 y, StringView text, u32 scale, u32 rgba) {
    const i32 s = static_cast<i32>(scale);
    const i32 w = static_cast<i32>(text.size()) * 4 * s + s;
    fill_rect(img, x, y, w, 7 * s, 0x000000B0u);
    i32 cx = x + s;
    for (const char ch : text) {
        if (const Glyph* g = glyph(ch)) {
            for (i32 row = 0; row < 5; ++row)
                for (i32 col = 0; col < 3; ++col)
                    if (g->rows[row] & (4 >> col)) fill_rect(img, cx + col * s, y + s + row * s, s, s, rgba | 0xFFu);
        }
        cx += 4 * s;
    }
}

DiffResult diff_images(const cook::Image& golden, const cook::Image& image, u32 threshold) {
    DiffResult r;
    r.heatmap = make_image(golden.width, golden.height, 0x000000FFu);
    const usize n = usize(golden.width) * golden.height;
    u64 sum = 0, changed = 0;
    for (usize i = 0; i < n; ++i) {
        const u8* a = &golden.rgba[i * 4];
        const u8* b = &image.rgba[i * 4];
        u32 worst = 0;
        for (u32 c = 0; c < 3; ++c) {
            const u32 d = static_cast<u32>(std::abs(int(a[c]) - int(b[c])));
            sum += d;
            if (d > worst) worst = d;
        }
        if (worst > r.max_channel_diff) r.max_channel_diff = worst;
        u8* h = &r.heatmap.rgba[i * 4];
        const u8 grey = static_cast<u8>((u32(a[0]) * 77 + u32(a[1]) * 150 + u32(a[2]) * 29) >> 10);  // a quarter of the luma
        h[0] = h[1] = h[2] = grey;
        if (worst > threshold) {
            ++changed;
            h[0] = static_cast<u8>(128 + worst / 2);
            h[1] = h[2] = 0;
        }
    }
    r.score = n ? static_cast<f64>(sum) / (static_cast<f64>(n) * 3.0 * 255.0) : 0.0;
    r.changed_fraction = n ? static_cast<f64>(changed) / static_cast<f64>(n) : 0.0;
    return r;
}

}  // namespace mce::mcp
