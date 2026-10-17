#pragma once
// HDR image writers for tests (M-VISUAL3 4): the cook and the goldens author their own .hdr / .exr sources.
//   make_rgbe_hdr  Radiance RGBE, flat (no run-length coding), grey pixels only: a pixel whose first two bytes
//                  are 2, 2 would read as a run-length scanline marker, and a grey pixel never starts so.
//   make_exr       OpenEXR 2.0, one scanline part, no compression, float A B G R channels (alphabetical, as the
//                  format wants), rows top first.
//   make_exr_tiled The same pixels as one-level tiles (uncompressed), tile x tile each, edge tiles clipped.
//   make_exr_header  Only the header (through its end byte) for a data window of w x h: a hostile-size fixture.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hdr_fixtures {

// `grey` holds w * h values, row-major, top row first.
inline std::string make_rgbe_hdr(unsigned w, unsigned h, const std::vector<float>& grey) {
    std::string s = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(h) + " +X " + std::to_string(w) + "\n";
    for (unsigned i = 0; i < w * h; ++i) {
        const float v = grey[i];
        unsigned char px[4] = {0, 0, 0, 0};
        if (v > 1e-32f) {
            int e = 0;
            const float m = std::frexp(v, &e);  // v = m * 2^e, m in [0.5, 1)
            const unsigned char b = static_cast<unsigned char>(m * 256.0f);
            px[0] = px[1] = px[2] = b;
            px[3] = static_cast<unsigned char>(e + 128);
        }
        s.append(reinterpret_cast<const char*>(px), 4);
    }
    return s;
}

namespace detail {
inline void p32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
inline void attr(std::string& s, const char* name, const char* type, const std::string& data) {
    s += name;
    s.push_back(0);
    s += type;
    s.push_back(0);
    p32(s, static_cast<std::uint32_t>(data.size()));
    s += data;
}
// The header of a w x h file; `tile` > 0 makes it a tiled file (version flag, "tiles" attribute).
inline std::string exr_header(unsigned w, unsigned h, unsigned tile = 0) {
    using namespace detail;
    std::string out;
    p32(out, 20000630);  // magic 0x762f3101
    out[0] = 0x76;
    out[1] = 0x2f;
    out[2] = 0x31;
    out[3] = 0x01;
    p32(out, tile ? 2u | 0x200u : 2u);  // version 2; bit 9 = tiled
    std::string ch;
    for (const char* name : {"A", "B", "G", "R"}) {
        ch += name;
        ch.push_back(0);
        p32(ch, 2);  // FLOAT
        p32(ch, 0);  // pLinear + reserved
        p32(ch, 1);  // x sampling
        p32(ch, 1);  // y sampling
    }
    ch.push_back(0);
    attr(out, "channels", "chlist", ch);
    attr(out, "compression", "compression", std::string(1, '\0'));
    std::string box;
    p32(box, 0);
    p32(box, 0);
    p32(box, w - 1);
    p32(box, h - 1);
    attr(out, "dataWindow", "box2i", box);
    attr(out, "displayWindow", "box2i", box);
    attr(out, "lineOrder", "lineOrder", std::string(1, '\0'));
    std::string f1;
    p32(f1, 0x3F800000u);  // 1.0f
    attr(out, "pixelAspectRatio", "float", f1);
    std::string c2;
    p32(c2, 0);
    p32(c2, 0);
    attr(out, "screenWindowCenter", "v2f", c2);
    attr(out, "screenWindowWidth", "float", f1);
    if (tile) {
        std::string t;
        p32(t, tile);
        p32(t, tile);
        t.push_back(0);  // ONE_LEVEL, round down
        attr(out, "tiles", "tiledesc", t);
    }
    out.push_back(0);  // end of header
    return out;
}
}  // namespace detail

inline std::string make_exr_header(unsigned w, unsigned h) { return detail::exr_header(w, h); }

// `rgba` holds w * h * 4 floats (R G B A per pixel), row-major, top row first.
inline std::string make_exr(unsigned w, unsigned h, const std::vector<float>& rgba) {
    using namespace detail;
    std::string out = exr_header(w, h);

    const std::uint64_t table_at = out.size();
    const std::uint64_t row_bytes = 8 + std::uint64_t(w) * 4 * 4;
    const std::uint64_t first = table_at + std::uint64_t(h) * 8;
    for (unsigned y = 0; y < h; ++y) {
        const std::uint64_t off = first + y * row_bytes;
        p32(out, static_cast<std::uint32_t>(off & 0xFFFFFFFFu));
        p32(out, static_cast<std::uint32_t>(off >> 32));
    }
    for (unsigned y = 0; y < h; ++y) {
        p32(out, y);
        p32(out, w * 16);
        for (int c : {3, 2, 1, 0}) {  // A, B, G, R
            for (unsigned x = 0; x < w; ++x) {
                std::uint32_t bits;
                std::memcpy(&bits, &rgba[(std::size_t(y) * w + x) * 4 + c], 4);
                p32(out, bits);
            }
        }
    }
    return out;
}

inline std::string make_exr_tiled(unsigned w, unsigned h, unsigned tile, const std::vector<float>& rgba) {
    using namespace detail;
    std::string out = exr_header(w, h, tile);
    const unsigned nx = (w + tile - 1) / tile, ny = (h + tile - 1) / tile;
    std::string chunks;
    std::vector<std::uint64_t> offs;
    const std::uint64_t first = out.size() + std::uint64_t(nx) * ny * 8;
    for (unsigned ty = 0; ty < ny; ++ty) {
        for (unsigned tx = 0; tx < nx; ++tx) {
            offs.push_back(first + chunks.size());
            const unsigned x0 = tx * tile, y0 = ty * tile;
            const unsigned tw = std::min(tile, w - x0), th = std::min(tile, h - y0);
            p32(chunks, tx);
            p32(chunks, ty);
            p32(chunks, 0);  // level x
            p32(chunks, 0);  // level y
            p32(chunks, tw * th * 16);
            for (unsigned y = 0; y < th; ++y) {
                for (int c : {3, 2, 1, 0}) {
                    for (unsigned x = 0; x < tw; ++x) {
                        std::uint32_t bits;
                        std::memcpy(&bits, &rgba[(std::size_t(y0 + y) * w + x0 + x) * 4 + c], 4);
                        p32(chunks, bits);
                    }
                }
            }
        }
    }
    for (std::uint64_t o : offs) {
        p32(out, static_cast<std::uint32_t>(o & 0xFFFFFFFFu));
        p32(out, static_cast<std::uint32_t>(o >> 32));
    }
    return out + chunks;
}

}  // namespace hdr_fixtures
