#pragma once
// Undelivered components: plain reflected data (MCE_COMPONENT). The scene authors them through the
// editor MCP server (tools/author_scene_mcp.py), the cook validates them, and the systems in
// undelivered.cpp run on them. Data-only (core + refl), so the cook can include it without the game.
//
// The stage is the visual-novel kit's (mce/kit/novel_components.h: StagePart entities and the one
// StoryState the save carries). What is Undelivered's own is the drawer: seven envelopes and the
// "delivered" stamps over them, drawn by the game's drawer screen.

#include "mce/kit/novel_components.h"
#include "mce/refl/macros.h"
#include "mce/types.h"

namespace undelivered {

using mce::u8;

enum class DrawerRole : u8 {
    Envelope,  // Sprite: an envelope on the drawer screen (index = letter - 1)
    Stamp,     // Sprite: the "delivered" stamp over an envelope (index = letter - 1)
};
MCE_ENUM(DrawerRole, Envelope, Stamp)

struct DrawerPart {
    DrawerRole role = DrawerRole::Envelope;
    u8 index = 0;
    u8 pad_[2] = {};
};
MCE_COMPONENT(DrawerPart, role, (index, Range{0, 6}, Help{"the letter's number - 1"}))

}  // namespace undelivered
