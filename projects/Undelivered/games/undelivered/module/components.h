#pragma once
// Undelivered components: plain reflected data (MCE_COMPONENT). The scene authors them through the
// editor MCP server (tools/author_scene_mcp.py), the cook validates them, and the systems in
// undelivered.cpp run on them. Data-only (core + refl), so the cook can include it without the game.
//
// A light novel on the 2D kit: every piece of the screen is an entity the scene places (a Sprite or
// a TextLabel) tagged with a UiPart that says which part it plays; the game writes textures, text
// and tints into them each tick. The whole story state is one NovelSave component on an entity with
// a SaveKey, every field Save-flagged, so the engine's save games carry a run with nothing else.

#include "mce/refl/macros.h"
#include "mce/types.h"

namespace undelivered {

using mce::f32;
using mce::i16;
using mce::i8;
using mce::u16;
using mce::u32;
using mce::u8;

// What an authored entity is for. `index` tells parts of the same role apart (line 0..3, cast slot
// 0..2, background layer 0..4, choice row 0..3, letter line 0..11, panel line 0..13, envelope 0..6).
enum class UiRole : u8 {
    None,
    Background,     // Sprite: one paper layer of the location (index = layer, far to near)
    Cast,           // Sprite: a character slot (0 left, 1 centre, 2 right)
    Dim,            // Sprite: the full-screen shade under choices and panels
    Fade,           // Sprite: the full-screen black used for scene changes
    Box,            // Sprite: the dialogue box
    NameTag,        // Sprite: the speaker tag
    NameText,       // TextLabel: the speaker's name
    Line,           // TextLabel: one of the four dialogue lines
    ChoiceRow,      // Sprite: a choice row (index 0..3)
    ChoiceText,     // TextLabel: a choice row's first line
    ChoiceText2,    // TextLabel: a choice row's second line
    LetterCard,     // Sprite: the paper a letter is read on
    LetterLine,     // TextLabel: one handwritten line (0..11)
    Panel,          // Sprite: the panel behind the title menu, the journal, the backlog, an ending
    PanelLine,      // TextLabel: one panel line (0..13)
    Envelope,       // Sprite: an envelope on the drawer screen (index = letter - 1)
    Stamp,          // Sprite: the "delivered" stamp over an envelope (index = letter - 1)
    Hud,            // TextLabel: the day and the drawer count, top right
    Title,          // TextLabel: the game's title on the title screen
    Glow,           // Sprite: the soft light behind the selected row or envelope
    Music,          // AudioSource: the looping music bed
    Ambience,       // AudioSource: the looping ambience bed
    Light,          // Light2D (Point): the lamp that moves with the time of day
    AmbientLight,   // Light2D (Ambient): the daylight the paper layers are lit by
    Camera,         // Camera2DComponent + Post2DComponent: the view and its LUT
    Save,           // the NovelSave carrier (with a SaveKey)
};
MCE_ENUM(UiRole, None, Background, Cast, Dim, Fade, Box, NameTag, NameText, Line, ChoiceRow, ChoiceText, ChoiceText2,
         LetterCard, LetterLine, Panel, PanelLine, Envelope, Stamp, Hud, Title, Glow, Music, Ambience, Light, AmbientLight,
         Camera, Save)

struct UiPart {
    UiRole role = UiRole::None;
    u8 index = 0;
    u8 pad_[2] = {};
};
MCE_COMPONENT(UiPart, role, (index, Range{0, 15}, Help{"which of several parts with this role"}))

// The story state (design §2.6), laid out as scalars because reflection has no array fields. The
// compiler (tools/compile_story.py) numbers the trust targets, flags and counters; the VM reads
// them by (kind, index) through the accessors in undelivered.cpp. `pc` is the statement the run
// resumes at (0 = no run in progress); `ret0..3` the call stack.
struct NovelSave {
    u16 pc = 0;
    u16 ret0 = 0, ret1 = 0, ret2 = 0, ret3 = 0;
    u8 depth = 0;
    u8 delivered = 0;   // bit n: letter n delivered (1..7)
    u8 read = 0;        // bit n: letter n read
    u8 endings_seen = 0;  // bit e: ending e reached in this slot
    i8 trust_mira = 0, trust_feride = 0, trust_selin = 0, trust_halil = 0, trust_yusuf = 0, trust_hasan = 0, trust_cemil = 0;
    u8 pad0_ = 0;
    u32 flags = 0;      // bit f: story::Flag f
    i16 resolve = 0, day = 0, drawer_letter = 0, drawer_action = 0, afternoon = 0;
    i16 aux0 = 0, aux1 = 0, aux2 = 0, aux3 = 0, aux4 = 0, aux5 = 0, aux6 = 0, aux7 = 0;
    i16 mira_days = 0, selin_days = 0, yusuf_days = 0, alone_days = 0;
    // The picture at the save point, so Continue rebuilds it: location, time, LUT, music, ambience
    // and the three cast slots (character 255 = empty).
    u8 bg = 0, bg_time = 0, bg_lut = 0, music = 0, ambience = 0;
    u8 cast0 = 255, cast1 = 255, cast2 = 255;
    u8 expr0 = 0, expr1 = 0, expr2 = 0;
    u8 pose0 = 0, pose1 = 0, pose2 = 0;
    u8 pad1_[3] = {};
    u32 pages = 0;      // pages read in this run (statistics; the journal shows them)
};
MCE_COMPONENT(NovelSave, (pc, Save, ReadOnly), (ret0, Save, ReadOnly), (ret1, Save, ReadOnly), (ret2, Save, ReadOnly),
              (ret3, Save, ReadOnly), (depth, Save, ReadOnly), (delivered, Save), (read, Save), (endings_seen, Save),
              (trust_mira, Save, Range{-3, 6}), (trust_feride, Save, Range{-3, 6}), (trust_selin, Save, Range{-3, 6}),
              (trust_halil, Save, Range{-3, 6}), (trust_yusuf, Save, Range{-3, 6}), (trust_hasan, Save, Range{-3, 6}),
              (trust_cemil, Save, Range{-3, 6}), (flags, Save), (resolve, Save), (day, Save), (drawer_letter, Save),
              (drawer_action, Save), (afternoon, Save), (aux0, Save), (aux1, Save), (aux2, Save), (aux3, Save), (aux4, Save),
              (aux5, Save), (aux6, Save), (aux7, Save), (mira_days, Save), (selin_days, Save), (yusuf_days, Save),
              (alone_days, Save), (bg, Save, ReadOnly), (bg_time, Save, ReadOnly), (bg_lut, Save, ReadOnly),
              (music, Save, ReadOnly), (ambience, Save, ReadOnly), (cast0, Save, ReadOnly), (cast1, Save, ReadOnly),
              (cast2, Save, ReadOnly), (expr0, Save, ReadOnly), (expr1, Save, ReadOnly), (expr2, Save, ReadOnly),
              (pose0, Save, ReadOnly), (pose1, Save, ReadOnly), (pose2, Save, ReadOnly), (pages, Save, ReadOnly))

}  // namespace undelivered
