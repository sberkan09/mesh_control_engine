#pragma once
// InputScript — scripted action states for runs without a keyboard (headless determinism runs,
// the editor MCP server's run_headless, a windowed capture of one moment). Segments say which
// actions are held, with what value, over which ticks; each tick is evaluated into an InputState
// with the InputSystem's edge rules (pressed/released on the tick the held state changes,
// held_time accumulated at the fixed step), the fast-path fields filled by action name ("Move",
// "Jump", ...) exactly as evaluate() fills them. Actions a tick does not name are released.
//
// Text form (mce_player -input <file>): one segment per line, '#' starts a comment.
//   <from>[-<to>] <Action>[=<value>] [<Action>[=<value>] ...]
//   10-70 Move=1,0 Jump          ticks 10..70: Move (1, 0) and Jump held
//   100 Fire                     tick 100 only (a press)
//   120- Move=-1,0               from tick 120 to the end of the run
// Ticks are the 1-based sim ticks of app::TickContext. A value is a number (an axis, or a button
// when non-zero), two numbers "x,y" (a 2D axis) or true/false. A named action with no value is
// held. Segments may overlap: later lines win for the same action.
//
// The script implements app::IInputSource, so AppDesc::input_source = &script feeds every tick.

#include "mce/app.h"
#include "mce/containers/array.h"
#include "mce/input/actions.h"
#include "mce/result.h"

namespace mce::input {

struct ScriptSegment {
    u64 from = 1;             // first tick (1-based)
    u64 to = ~u64(0);         // last tick, inclusive (~0: to the end)
    StringId action;
    FixedString<32> name;     // the action's spelling, for logs
    f32 x = 1.f, y = 0.f;     // the action's value while the segment is active
};

class InputScript final : public app::IInputSource {
public:
    static constexpr u32 kTickHz = 60;

    // Holds `action` at (x, y) over ticks from..to (inclusive). Returns false for an empty name
    // or an inverted range.
    bool add(StringView action, u64 from, u64 to, f32 x = 1.f, f32 y = 0.f);
    // Parses the text form above (replaces nothing: segments append). Errc::ParseError names the
    // line and what was expected in the error message.
    Result<void> parse(StringView text);
    void clear();

    Span<const ScriptSegment> segments() const { return segments_.span(); }
    bool empty() const { return segments_.empty(); }
    // The last tick any bounded segment covers (0 when there is none, or only open-ended ones).
    u64 last_tick() const;

    // Evaluates `tick` into the snapshot (edges against the previously sampled tick: sequential
    // calls get presses and releases; a jump in tick numbers starts fresh).
    const InputState& evaluate(u64 tick);
    const InputState* sample_for_tick(u64 tick, u64 tick_close_us) override;
    const InputState& state() const { return state_; }

private:
    Array<ScriptSegment> segments_;
    InputState state_;
    InputState prev_;
    u64 last_evaluated_ = 0;
};

}  // namespace mce::input
