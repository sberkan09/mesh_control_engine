#pragma once
// A small RGBA picture shown in an ImGui panel as a user texture that lives as long as the editor (the Material and
// Particle panels' live swatches). Call with the same `tex` each time the picture changes; draws nothing itself.

#include <imgui.h>
#include <imgui_internal.h>  // RegisterUserTexture, ImTextureDataQueueUpload

#include <cstring>

#include "mce/cook/image.h"

namespace mce::editor {

inline void upload_swatch(ImTextureData*& tex, const cook::Image& img, u32 px) {
    if (!tex) {
        tex = IM_NEW(ImTextureData)();
        tex->Create(ImTextureFormat_RGBA32, static_cast<int>(px), static_cast<int>(px));
        std::memcpy(tex->GetPixels(), img.rgba.data(), img.rgba.size());
        ImGui::RegisterUserTexture(tex);
    } else if (tex->Status == ImTextureStatus_OK || tex->Status == ImTextureStatus_WantUpdates) {
        std::memcpy(tex->GetPixels(), img.rgba.data(), img.rgba.size());
        ImTextureDataQueueUpload(tex, 0, 0, static_cast<int>(px), static_cast<int>(px));
    }
}

}  // namespace mce::editor
