#pragma once
// Golden-image check (FR-DBG-020): a read-back frame against tests/render_gl/golden/<os>-<backend>/<name>.png.
// Dev-machine-only, never a CI gate: the drivers differ, so each machine keeps its own baseline and a
// missing golden is written and passes ("auto-rebaselined by convention"). MCE_UPDATE_GOLDENS=1 rewrites
// every golden it meets. The threshold is perceptual, not byte equality: the frame is split into 8x8
// blocks and every block's mean luminance must stay within 0.5% of full scale of the golden's. A
// mismatch writes <name>.actual.png and <name>.diff.png (|luma difference| x 8) next to the build.
// A vendor directory <os>-<backend>-<vendor>/ (vendor "nvidia" today, from the device name) holds the goldens a
// GPU family renders differently from the software rasterizer the repo goldens come from; a golden there wins
// over the plain directory's for that vendor. Only goldens measured identical across that vendor's GPUs go there
// (docs/milestones/M-GPU-VERIFY.md).

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "mce/cook/image.h"
#include "mce/core.h"
#include "mce/platform/window.h"

namespace mce::golden {

inline constexpr u32 kBlock = 8;
inline constexpr f64 kMaxBlockDiff = 0.005;  // of full scale

inline std::string backend_dir() {
#if defined(__APPLE__)
    const char* os = "macos";
#elif defined(_WIN32)
    const char* os = "windows";
#else
    const char* os = "linux";
#endif
#if defined(__APPLE__)
    const char* native = "metal";
#else
    const char* native = "gl";
#endif
    return std::string(os) + "-" + (platform::gpu_api() == platform::GpuApi::SdlGpu ? "sdlgpu" : native);
}

// "nvidia" when the device name says so, else empty (no vendor directory).
inline std::string vendor_of(const char* device_name) {
    std::string n = device_name ? device_name : "";
    for (char& ch : n) ch = char(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    return n.find("nvidia") != std::string::npos ? "nvidia" : "";
}

inline f64 luma(const u8* p) { return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; }

inline bool write_png(const std::string& path, u32 w, u32 h, Span<const u8> rgba) {
    Array<byte> png;
    if (!cook::encode_png(w, h, rgba, png)) return false;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    return bool(f);
}

struct Comparison {
    f64 worst = 0.0;  // the largest block-mean luminance difference, of full scale
    u32 failing = 0;  // blocks over kMaxBlockDiff
    u32 blocks = 0;
};

inline Comparison compare(u32 w, u32 h, Span<const u8> a, Span<const u8> b) {
    Comparison c;
    for (u32 by = 0; by < h; by += kBlock) {
        for (u32 bx = 0; bx < w; bx += kBlock) {
            f64 sa = 0.0, sb = 0.0;
            u32 n = 0;
            for (u32 y = by; y < by + kBlock && y < h; ++y) {
                for (u32 x = bx; x < bx + kBlock && x < w; ++x) {
                    const usize i = (usize(y) * w + x) * 4;
                    sa += luma(&a[i]);
                    sb += luma(&b[i]);
                    ++n;
                }
            }
            const f64 d = std::fabs(sa - sb) / (f64(n) * 255.0);
            c.worst = d > c.worst ? d : c.worst;
            c.failing += d >= kMaxBlockDiff;
            ++c.blocks;
        }
    }
    return c;
}

// Checks `rgba` (w x h, top row first) against the golden `name`; `device_name` picks a vendor directory.
inline void check(const char* name, u32 w, u32 h, Span<const u8> rgba, const char* device_name = nullptr) {
    std::string dir = std::string(MCE_GOLDEN_DIR) + "/" + backend_dir();
    const std::string vendor = vendor_of(device_name);
    if (!vendor.empty() && std::filesystem::exists(dir + "-" + vendor + "/" + name + ".png")) dir += "-" + vendor;
    const std::string path = dir + "/" + name + ".png";
    const char* update = std::getenv("MCE_UPDATE_GOLDENS");
    std::ifstream in(path, std::ios::binary);
    if (!in || (update && update[0] == '1')) {
        REQUIRE(write_png(path, w, h, rgba));
        MESSAGE("golden written: " << path);
        return;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Result<cook::Image> img = cook::decode_png(Span<const byte>(reinterpret_cast<const byte*>(bytes.data()), bytes.size()));
    REQUIRE(img.has_value());
    if (!img) return;
    const cook::Image& g = img.value();
    REQUIRE(g.width == w);
    REQUIRE(g.height == h);
    if (g.width != w || g.height != h) return;
    const Comparison c = compare(w, h, rgba, g.rgba.span());
    INFO(path << ": " << c.failing << " of " << c.blocks << " blocks over 0.5%, worst " << c.worst * 100.0 << "%");
    CHECK(c.failing == 0);
    if (c.failing == 0) return;
    const std::string out = std::string(MCE_GOLDEN_OUT) + "/" + backend_dir() + "/" + name;
    Array<u8> diff;
    diff.resize(rgba.size());
    for (usize i = 0; i < rgba.size(); i += 4) {
        const f64 d = std::fabs(luma(&rgba[i]) - luma(&g.rgba[i])) * 8.0;
        diff[i] = diff[i + 1] = diff[i + 2] = u8(d > 255.0 ? 255.0 : d);
        diff[i + 3] = 255;
    }
    write_png(out + ".actual.png", w, h, rgba);
    write_png(out + ".diff.png", w, h, diff.span());
    MESSAGE("actual and diff written next to " << out);
}

}  // namespace mce::golden
