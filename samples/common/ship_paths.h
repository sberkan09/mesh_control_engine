#pragma once
// Shared by the shippable samples: where a file the build produces lives at run time. A shipped
// build (scripts/ship.py) puts it next to the executable; a dev build falls back to the build-tree
// path the CMake target compiled in. An explicit convar value always wins.

#include <cstdio>

#include "mce/core.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"

namespace mce::samples {

inline void resolve_shipped_path(StringView override_path, StringView file_name, const char* dev_path,
                                 FixedString<1024>& out) {
    out.clear();
    if (!override_path.empty()) {
        out.append(override_path);
        return;
    }
    out.append(platform::base_path());
    out.append(file_name);
    if (std::FILE* f = std::fopen(out.c_str(), "rb")) {
        std::fclose(f);
        return;
    }
    out.clear();
    out.append(dev_path);
}

// Sets the window icon from a cooked RGBA8 texture in the mounted pack (no mips needed); a missing
// icon only logs. The asset is released again: the platform copies the pixels.
inline void apply_window_icon(platform::Window& window, asset::AssetSystem& assets, StringView path) {
    for (asset::AssetKey k = 0; k < assets.asset_count(); ++k) {
        if (assets.info(k).path != path) continue;
        auto h = assets.load_sync<asset::Texture>(k);
        if (!h) break;
        auto tex = asset::TextureView::read(assets.resolve(h.value()));
        bool ok = false;
        if (tex && !tex.value().mips.empty()) {
            const Span<const byte> px = tex.value().pixels(0);
            ok = window.set_icon(tex.value().mips[0].width, tex.value().mips[0].height,
                                 Span<const u8>(reinterpret_cast<const u8*>(px.data()), px.size()));
        }
        assets.release(asset::AnyHandle(h.value()));
        if (ok) return;
        break;
    }
    MCE_LOG_WARN("no usable window icon '{}' in the pack", path);
}

}  // namespace mce::samples
