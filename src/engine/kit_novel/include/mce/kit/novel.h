#pragma once
// mce_kit_novel (M-NOVEL, opt-in): a visual-novel kit and a branching-dialogue VM.
//
//   mce_add_project(my_novel ... DEPS mce::mce_kit_novel)        // link it (the SDK installs it too)
//   python3 <sdk>/bin/mce story compile <project>                 // story/*.story -> a header
//
//   #include "story_data.h"                                        // the compiled program
//   ResultV install(World& w) {
//       novel::NovelHooks hooks;                                   // optional: the game's own screens
//       return novel::install(w, my_novel::story::kProgram, hooks);
//   }
//
// What the kit runs (docs/user-guide/21-novels-and-dialogue.md):
//   - the story VM over a compiled Program: pages of text, choices with gates (conditions with
//     and/or over flags and counters), labels, jumps, calls, flags and counters, letters, endings,
//     checkpoints (the autosave), fades and waits, and game commands (@<name> the manifest
//     declares, run by NovelHooks::command)
//   - the stage: typewriter reveal, the backlog, music and ambience beds, cast slots with
//     expressions and poses, background layers lit by the time of day, the colour grade, the
//     title menu (New game / Continue / extras) and a playthrough bot (`autopilot`)
//   - NPC dialogue outside a novel stage: a Dialogue component runs a program from a label with
//     the same VM; game systems read and write its variables (dialogue section below)
//
// Everything is tick-driven (60 Hz) from the input snapshot, so a run is deterministic and a
// headless run plays the same as a windowed one.

#include "mce/asset/guid.h"
#include "mce/ecs/world.h"
#include "mce/fixed_string.h"
#include "mce/kit/novel_components.h"
#include "mce/math/math.h"

namespace mce::asset {
class AssetSystem;
}
namespace mce::loc {
class Localization;
struct Arg;
}
namespace mce::input {
struct InputState;
}
namespace mce::kit {
struct ViewInfo;
}

namespace mce::novel {

inline constexpr u16 kNone = 0xFFFF;

// ---- the compiled program (mce story compile writes one as a header) ---------------------------

enum class OpKind : u16 {
    Nop,
    Say,         // a speaker (kNone = narration), b expression (kNone = keep), c..f line text ids (0 = none)
    Choice,      // a first option, b count
    Jump,        // a target
    Call,        // a target (returns to the next statement)
    Return,      // pops the call stack (an empty stack: the next statement)
    If,          // a condition, b target
    Add,         // a variable, b delta (i16)
    Set,         // a variable, b value (i16)
    Bg,          // a location, b time, c lut
    Music,       // a music id (0 = none)
    Ambience,    // a ambience id (0 = none)
    Sfx,         // a sfx id
    Show,        // a slot, b character, c expression, d pose
    Hide,        // a character (kNone = everyone)
    Expr,        // a character, b expression
    Pose,        // a character, b pose
    Letter,      // a letter number, b the op of its text (a call)
    LetterPage,  // a first letter line, b count
    Checkpoint,  // the autosave and the checkpoint panel (Undelivered's night journal)
    Ending,      // a ending id
    End,         // the run is over (back to the title)
    Fade,        // a timed black
    Wait,        // a ticks
    Command,     // a game command id, b..f arguments (NovelHooks::command)
    Tag,         // a text id: a tag of the next page (#tag in the script; Dialogue::tags)
};

struct Op {
    u16 kind, a, b, c, d, e, f;
};

// A choice option. text2: the second line (0 = none). hidden: not listed unless its condition
// passes (a shown option whose condition fails is greyed out).
struct Option {
    u16 text, text2, target, cond;
    u8 tag, hidden;
};

// Conditions: a run of clauses from `cond` to the first with join End. `and` binds tighter than
// `or`: (a and b) or c. Clause 0 is "always".
enum class Test : u8 { Always, Set, Clear, Ge, Le, Eq, Ne, Gt, Lt };
enum class Join : u8 { End, And, Or };
struct Clause {
    u16 var;
    i16 n;
    Test test;
    Join join;
};

// A variable reference: bit 15 set = counter (low bits its index), else a flag index.
inline constexpr u16 kCounterBit = 0x8000;

struct VarInfo {
    const char* name;
    u16 ref;
    i16 min, max;  // counters are clamped to this range
};

struct CharacterInfo {
    const char* id;    // the script's name (lower case) and the texture prefix
    const char* name;  // what the name tag shows
    u8 cast;           // its row in the cast sprite table (textures/cast/<id>_*), 255: speaks only
};

struct EndingInfo {
    const char* id;
    const char* title;
};

// The look of a time of day: tints of the background layers and the cast, the ambient light,
// and the lamp (colour, position in metres, intensity).
struct TimeLook {
    const char* id;
    math::Color paper, cast, ambient, lamp;
    f32 lamp_x, lamp_y, lamp_intensity;
};

struct LabelInfo {
    const char* name;
    u16 op;
};

// Translations (M-MOBILE step 11e; story.json "localize"). The compiler wraps each language's text
// like the base text and appends it to Program::text; these map what the run shows (base text ids,
// the same in every language, so the run and its hash are too) to what the box shows instead.
struct PageText {
    u16 base[4];  // a page's base line ids (a Say op's c..f); sorted
    u16 line[4];  // the translated lines
};
struct OptionText {
    u16 base, base2;  // an option's base text ids; sorted
    u16 text, text2;
};
struct LocaleText {
    const char* locale;  // a string table's tag ("tr", "pt-BR")
    const PageText* pages;
    u16 page_count;
    const OptionText* options;
    u16 option_count;
    const u16* names;  // a translated name per character (text ids; 0 = the base name)
};

struct Program {
    const Op* ops = nullptr;
    u16 op_count = 0;
    const Option* options = nullptr;
    u16 option_count = 0;
    const Clause* clauses = nullptr;
    u16 clause_count = 0;
    const char* const* text = nullptr;  // [0] = ""
    u16 text_count = 0;
    const u16* letter_lines = nullptr;  // text ids
    u16 letter_line_count = 0;
    const u16* letters = nullptr;  // letters[n] = the op of letter n's text ([0] unused)
    u16 letter_count = 0;          // highest letter number + 1
    const LabelInfo* labels = nullptr;
    u16 label_count = 0;
    const CharacterInfo* characters = nullptr;
    u16 character_count = 0;
    const char* const* expressions = nullptr;
    u16 expression_count = 0;
    const char* const* poses = nullptr;
    u16 pose_count = 0;
    u16 slot_count = 0;
    const char* const* locations = nullptr;  // [0] = "none"
    u16 location_count = 0;
    const TimeLook* times = nullptr;
    u16 time_count = 0;
    const char* const* luts = nullptr;
    u16 lut_count = 0;
    const char* const* music = nullptr;  // [0] = "none"
    u16 music_count = 0;
    const char* const* ambience = nullptr;  // [0] = "none"
    u16 ambience_count = 0;
    const char* const* sfx = nullptr;  // [0] = "none"
    u16 sfx_count = 0;
    const EndingInfo* endings = nullptr;  // [0] = none
    u16 ending_count = 0;
    const char* const* tags = nullptr;  // an option without {tag} carries tags[0]
    u16 tag_count = 0;
    const u16* bot_prefer = nullptr;  // tag_count rows of 4 tag ids (kNone-padded): the bot's preference per policy
    const VarInfo* vars = nullptr;
    u16 var_count = 0;
    const char* const* commands = nullptr;
    u16 command_count = 0;
    u16 start = 0;  // the op the 'start' label names
    u16 lines_per_page = 4;
    u16 letter_lines_per_page = 12;
    const char* title = "";
    const char* extras = "";  // the title menu's third row (an extras page); "" = none
    u16 sfx_page = 0, sfx_move = 0, sfx_confirm = 0, sfx_letter = 0;
    u16 title_location = 0, title_time = 0, title_lut = 0, title_music = 0, title_ambience = 0;
    const char* background_normal_map = nullptr;  // an asset path, or null
    const char* text_font = nullptr;  // story.json's text.font as a pack path: the box's font (null: the pack's first)
    // Ink: the box's speech and narration lines, the choice rows' text (normal, selected,
    // disabled), and the row sprites' tint (multiplied by 0.78 / 1 / 0.45 the same way).
    math::Color ink_speech{0.16f, 0.12f, 0.1f, 1}, ink_narration{0.36f, 0.29f, 0.23f, 1};
    math::Color ink_row{0.3f, 0.255f, 0.225f, 1}, ink_row_selected{0.12f, 0.102f, 0.09f, 1}, ink_row_disabled{0.55f, 0.4675f, 0.4125f, 1};
    math::Color row_tint{1, 1, 1, 1};
    const LocaleText* locales = nullptr;  // the translations (none without "localize")
    u16 locale_count = 0;
    const char* base_locale = "en";       // the language the script is written in
};

// The label's op, or kNone.
u16 find_label(const Program& p, StringView name);
// A variable's reference by its script name ("trust.mira", "day"), or kNone.
u16 find_var(const Program& p, StringView name);

// ---- translations (presentation only: the run never reads them) --------------------------------

// The translation the player's language chain picks (loc::Localization::chain(): the first locale
// that is the base language or has a table), or null for the base text: no Localization, no
// translations, or a chain that reaches the base language first.
const LocaleText* story_locale(const Program& p, const loc::Localization* l10n);
const LocaleText* story_locale(const Program& p, World& world);  // the world's Localization resource

// What to show for a page's base lines / an option / a speaker in `lt` (null: the base text).
void localized_page(const Program& p, const LocaleText* lt, const u16 base[4], u16 out[4]);
void localized_option(const Program& p, const LocaleText* lt, const Option& o, u16& text, u16& text2);
const char* localized_speaker(const Program& p, const LocaleText* lt, u8 character);
// The kit's own words (the novel.* keys) and the story's titles (story.title, story.extras,
// story.ending.<id>: the story compile writes them into the base table) in the player's language
// (SBA-59). The pattern is the text of `key` on the language chain (with `touch`, "<key>.touch"
// first: "Tap" where the keys say "Enter"), else `fallback`; formatted with `args` (loc::format)
// into out[cap], NUL-terminated. A table that lacks a key shows the English words, never the key.
void kit_text(const loc::Localization* l10n, StringView key, StringView fallback, char* out, u32 cap,
              Span<const loc::Arg> args = {}, bool touch = false);
// A count: the key's plural form for `count` (Localization::plural) when a table on the chain has
// the key, else `one` / `other` with {count}.
void kit_count(const loc::Localization* l10n, StringView key, i64 count, StringView one, StringView other, char* out,
               u32 cap);
// The typewriter runs on the base page (the sim's clock): `revealed` of its `base_points` mapped
// onto a shown page of `shown_points`, so both finish together.
u32 scale_reveal(u32 revealed, u32 base_points, u32 shown_points);

// ---- variables and conditions ----------------------------------------------------------------

i32 get_var(const StoryState& s, u16 ref);
void set_var(StoryState& s, const Program& p, u16 ref, i32 value);  // counters clamp to their range
bool test(const StoryState& s, const Program& p, u16 cond);         // a condition id (Clause index)

// ---- the stage ---------------------------------------------------------------------------------

// Where the run is between two inputs.
enum class Mode : u8 {
    Boot,        // the first tick: tables, the save entity
    Title,       // the title menu
    Extras,      // the title's extras page (endings and letters seen)
    Reading,     // a page is on the box
    Choice,      // choice rows are up
    Letter,      // a letter page is on the card
    Game,        // a game command owns the screen (NovelHooks)
    Checkpoint,  // the checkpoint panel (after the autosave)
    Backlog,     // the last pages
    Ending,      // the ending card
    Fade,        // a timed black
    Wait,        // a timed hold
    Done,        // nothing to run (no StoryState in the scene)
};

struct LogEntry {
    u8 speaker = 255;
    u16 line[4] = {};
};

// The input a tick carries, edge-detected (the stock player's default map: arrows / WASD / stick
// navigate, Enter / Space / A confirm, Esc / B back, Fire held skips, the wheel opens the backlog).
struct Keys {
    bool advance = false, up = false, down = false, left = false, right = false, skip = false, log = false, back = false;
};

// What the choice rows show (text, second line, selectable).
struct RowText {
    const char* a = nullptr;
    const char* b = nullptr;
    bool enabled = true;
};

// The stage's look this tick. The kit fills it from the run; NovelHooks::screen may change it
// while a game command owns the screen (Mode::Game).
struct ScreenLook {
    u16 location = 0, time = 0, lut = 0, music = 0, ambience = 0;
    bool cast = true;         // the cast slots are visible
    bool box = false;         // the dialogue box is up
    bool rows = false;        // the choice rows are up (NovelHooks::row gives their text)
    bool glow_owned = false;  // the game places the Glow itself
    const char* lines[4] = {};  // the box's lines while a game screen is up
    math::Color line_color{0.3f, 0.24f, 0.19f, 1};
};

enum class Panel : u8 { Checkpoint, Ending, Extras, Backlog };

struct NovelState;

// What a hook sees: the frame, the kit's runtime state, the story state and the program.
struct Context {
    Frame& f;
    NovelState& n;
    StoryState& s;
    const Program& p;
};

// The game's hooks, all optional (null: the kit's default). `user` is the game's own pointer.
struct NovelHooks {
    void* user = nullptr;
    // @<command> (story.json "commands"). Return true to wait: the game owns the screen
    // (Mode::Game) until it calls resume(). The op is the statement (b..f its arguments); the pc
    // already points past it.
    bool (*command)(Context& c, u16 command, const Op& op) = nullptr;
    // Mode::Game: the player's input, or the bot's turn (autopilot) each tick.
    void (*game_input)(Context& c, const Keys& keys) = nullptr;
    void (*game_bot)(Context& c) = nullptr;
    // Before the stage is drawn: the game may change the look (it must while Mode::Game).
    void (*screen)(Context& c, ScreenLook& look) = nullptr;
    // After the stage is drawn: the game's own parts (its components).
    void (*present)(Context& c, const ScreenLook& look) = nullptr;
    // A choice row while a game screen has rows up.
    RowText (*row)(Context& c, u32 row) = nullptr;
    // A panel line: return true when written (else the kit's line).
    bool (*panel_line)(Context& c, Panel panel, u32 line, char* out, u32 cap) = nullptr;
    // The HUD line ("" = none).
    void (*hud)(Context& c, char* out, u32 cap) = nullptr;
    // A run starts (New game, or a label from novel.label): reset the game's own state.
    void (*new_run)(Context& c) = nullptr;
};

// The runtime state the kit's systems share (a resource; not saved: the save is StoryState).
// A host or test sets `autopilot` before the first tick: 0 plays by input, 1 + a tag index picks
// options by that tag's preference row (Program::bot_prefer) and never waits.
struct NovelState {
    const Program* program = nullptr;
    NovelHooks hooks;
    u8 autopilot = 0;
    bool autosave = true;      // write the autosave slot at each checkpoint (tests turn it off)
    bool instant_text = false;
    bool default_stage = true;  // spawn a stage when the scene has no StagePart entities
    Mode mode = Mode::Boot;
    Entity save_entity;
    asset::AssetSystem* assets = nullptr;
    FixedString<64> save_slot;  // the autosave slot (install: "novel_autosave")
    u32 module_version = 1;     // written into the autosave's header

    // asset tables (GUIDs by name, from the pack's paths)
    static constexpr u32 kLocations = 32, kLayers = 5, kCast = 16, kExpr = 8, kPoses = 4, kSounds = 32, kLuts = 8;
    asset::Guid bg[kLocations][kLayers];
    u8 bg_count[kLocations] = {};
    asset::Guid cast[kCast][kExpr][kPoses];
    asset::Guid lut[kLuts];
    asset::Guid music[kSounds];
    asset::Guid ambience[kSounds];
    asset::Guid sfx[kSounds];
    asset::Guid normal_map;
    asset::Guid font;  // Program::text_font, else the first font in the pack (the default stage's)

    // the page on the box
    u8 page_speaker = 255;
    u16 page_line[4] = {};
    u32 page_chars = 0;  // code points over the lines
    f32 revealed = 0.0f;

    // the choice (or the title menu / a game screen's rows, which reuse them)
    u16 opt_first = 0, opt_count = 0;
    u8 vis[4] = {};  // option indices shown, in order
    u8 vis_count = 0;
    u8 cursor = 0;

    // the letter card
    u16 letter_first = 0, letter_count = 0;
    u16 letter_number = 0;

    // a game command's screen
    u16 command = 0;

    // timers and transitions
    u32 hold_ticks = 0;
    f32 fade = 0.0f;  // black overlay alpha
    f32 cast_alpha[kMaxSlots] = {};
    u32 ticks = 0;
    u32 skip_cooldown = 0;
    f32 prev_move_x = 0.0f, prev_move_y = 0.0f;

    // the backlog ring
    LogEntry log[64];
    u8 log_head = 0, log_count = 0, log_scroll = 0;

    // statistics (tests, the checkpoint panel)
    u32 pages = 0, choices = 0, letters_read = 0, saves = 0, ops = 0;
    u8 ending = 0;  // the ending reached (0 = none yet)
    u32 endings_this_session = 0;
    bool finished = false;  // an @end ran

    // presentation (SBA-59): the last read-on was a finger (the panels say "Tap"), and the title
    // menu's rows in the player's language
    bool touch = false;
    char title_rows[3][64] = {};

    // the screen reader (M-ACCESS): a hash of what the stage last spoke (a finished page, the
    // selected row, a letter, a panel), so each is said once
    u32 spoken = 0;
};

// Registers the kit's components and systems and the NovelState resource (an existing one keeps
// its autopilot / autosave / instant_text / default_stage). The program must outlive the world.
// Systems: "novel_setup" and "novel_step" (FixedUpdate_Pre), "novel_present" (FixedUpdate_Post).
ResultV install(World& world, const Program& program, const NovelHooks& hooks = {});

// The StoryState of the scene's save entity (null before the first tick / without one).
StoryState* story_state(World& world);

// Starts a fresh run on the next tick (keeps the slot's endings_seen).
void new_game(World& world);

// Runs statements until one waits for the player (or the bot, next tick). A game command's
// screen calls it when it is done.
void resume(Context& c);

// The UI sound by program sfx id at `gain_db` (bus UI); 0 / a missing clip: nothing.
void play_sfx(Context& c, u16 sfx, f32 gain_db);

// The stock player's default map read into edge-detected keys (NovelState keeps the last stick).
Keys read_keys(Frame& f, NovelState& n);

// UTF-8 helpers the stage uses (code points, and the prefix holding `points` of them).
u32 utf8_points(StringView s);
StringView utf8_prefix(StringView s, u32 points);

// ---- NPC dialogue (#154) ------------------------------------------------------------------------

// A conversation on an entity: the VM runs the program from `start` (a label) when `active` is
// set, stops on each page and choice, and clears `active` at @end, @return with an empty stack
// or the end of the program. Variables are the world's StoryState's (the first entity carrying
// one; dialogue spawns one with SaveKey kDialogueSaveKey when the scene has none), so quests and
// novels share flags. Every field is Save-flagged: a save mid-conversation resumes on the same
// line. Stage statements (@bg, @show, @music ...) are skipped; @sfx plays; #tags of a page land
// in `tags` (text ids) for the game's portrait and sound choices.
struct Dialogue {
    u16 pc = 0;
    u16 ret[4] = {};
    u8 depth = 0;
    bool active = false;
    bool waiting_choice = false;
    u8 option_count = 0;      // visible options while waiting_choice
    u8 speaker = 255;         // the page's speaker (character index; 255 = narration)
    u8 expression = 255;      // its #portrait / Speaker(expr) (255 = none)
    u8 cursor = 0;            // the highlighted option
    u8 tag_count = 0;
    u16 line[4] = {};         // the page's text ids
    u16 options[4] = {};      // option indices (into Program::options) shown, in order
    u16 tags[4] = {};         // text ids of the page's #tags
    u16 opt_first = 0;
    u16 reveal = 0;           // the typewriter: ticks since the page came up (kRevealAll: the whole page)
};
MCE_COMPONENT(Dialogue, (pc, Save), (ret, Save), (depth, Save), (active, Save), (waiting_choice, Save),
              (option_count, Save), (speaker, Save), (expression, Save), (cursor, Save), (tag_count, Save), (line, Save),
              (options, Save), (tags, Save), (opt_first, Save), (reveal, Save))

inline constexpr u16 kRevealAll = 0xFFFF;

inline constexpr u32 kDialogueSaveKey = 0x4E564C31;  // "NVL1"

// Requests on a Dialogue (events, FixedUpdate_Pre): start a conversation at a label, advance a
// page, pick an option by its visible index, or move the highlight. Games raise them from input
// (DialogueInput does it from the stock map) or from AI.
struct DialogueStart {
    Entity entity;
    FixedString<48> label;
};
struct DialogueAdvance {
    Entity entity;
};
struct DialogueChoose {
    Entity entity;
    u8 option = 0;
};

// Marks the entity whose Dialogue the stock-map input drives: Enter shows the rest of a page that
// is still typing, then advances or picks; up/down move the highlight. One per world is typical:
// the conversation on screen (the default dialogue box, mce/kit/novel_ui.h, shows that one).
struct DialogueInput {
    u8 enabled = 1;
    u8 pad_[3] = {};
};
MCE_COMPONENT(DialogueInput, enabled)

// Registers Dialogue, DialogueInput, the request events and the "dialogue_run" system
// (FixedUpdate_Pre). The program must outlive the world. Independent of install(): a game that
// has no novel stage calls only this.
ResultV install_dialogue(World& world, const Program& program);

// The dialogue's program (install_dialogue), the text of a text id, the name of a speaker.
const Program* dialogue_program(World& world);
const char* text(const Program& p, u16 id);
const char* speaker_name(const Program& p, u8 character);

// Starts / advances / chooses right away (outside a system, or from one that owns the entity).
bool start_dialogue(World& world, Entity e, StringView label);
void advance_dialogue(World& world, Entity e);
void choose_dialogue(World& world, Entity e, u8 visible_option);

// The world's story variables (the StoryState dialogue reads and writes), or null.
StoryState* dialogue_vars(World& world);

// The typewriter: points (UTF-8 code points) a page has, and how many of them show now. The
// speed is points per second of sim time (default 45; 0 shows whole pages).
u32 dialogue_page_points(const Program& p, const Dialogue& d);
u32 dialogue_revealed(World& world, const Dialogue& d);
void set_dialogue_speed(World& world, f32 points_per_second);

// ---- pointer input (M-MOBILE 11f) -----------------------------------------------------------------

// Taps and clicks go through the input snapshot, so a replay and a headless run see them: the
// first touch that went down this tick, else the "Point" action (the stock map's left mouse
// button) at the mouse. On the novel stage a tap on a choice row (or a title menu row) picks it
// and a tap anywhere else reads on; on a DialogueInput conversation a tap on an option row picks
// it and a tap on the box finishes the typewriter, then advances. A pick by pointer plays the
// Selection haptic (mce/platform/haptics.h). A tick with a tap ignores the keys' confirm, so a
// touch the stock map also reads as Jump does not pick twice.

// Where the pointer pressed this tick, in device pixels (origin top-left); false: no press.
bool pointer_pressed(const input::InputState& in, const kit::ViewInfo& view, math::vec2& px);

// The default dialogue box's geometry, in UI points at scale 1 (the box scales them by
// ViewInfo::ui_scale and lays out inside the safe area). The sim hit-tests the same rectangles
// the box draws (mce/kit/novel_ui.h), so a tap lands where the player sees the row.
struct DialogueLayout {
    f32 text_px = 26.0f;
    f32 box_height = 190.0f;
};

struct DialogueRect {
    f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // device pixels, y down
    bool contains(math::vec2 p) const { return p.x >= x0 && p.y >= y0 && p.x < x1 && p.y < y1; }
};

// The box and its four option rows on `view`'s screen.
struct DialogueRects {
    DialogueRect box;
    DialogueRect rows[4];
};
DialogueRects dialogue_rects(const DialogueLayout& layout, const kit::ViewInfo& view);

// The layout dialogue hit-tests with (install_dialogue's default until set).
void set_dialogue_layout(World& world, const DialogueLayout& layout);
DialogueLayout dialogue_layout(World& world);

}  // namespace mce::novel
