#pragma once
// Performance budgets per target (M-PERF step 2): what a frame may cost on a desktop, a phone or in a
// browser, and the verdict of a capture's summary against one (mce/perf_capture.h).
//
// A project sets its own in its .mceproj ("budgets": {"mobile": {"draws": 300}}); what it leaves out
// keeps the defaults below. Times are judged on their p95 and counters on their worst frame. Times
// are the capture machine's, so a desktop capture judged against "mobile" says what the frame asks
// of the GPU and CPU (draws, triangles, sim time), not how the phone will run it; the counters are
// what carry across machines. A limit of 0 is "no limit".

#include "mce/perf_capture.h"

namespace mce::perf {

enum class Target : u8 { Desktop, Mobile, Web, Count };
inline constexpr usize kTargetCount = static_cast<usize>(Target::Count);

const char* target_name(Target t);         // "desktop", "mobile", "web"
bool parse_target(StringView s, Target& out);

struct Budget {
    // Milliseconds, judged on the capture's p95.
    f32 frame_ms = 0, sim_ms = 0, render_ms = 0, gpu_ms = 0;
    // Counters, judged on the capture's worst frame.
    u32 draws = 0, triangles = 0, sprites = 0;
    u64 upload_bytes = 0, heap_allocations = 0;
    // Static rules (project_check): distinct sprite textures in one scene (each one breaks a batch)
    // and the largest texture edge in pixels.
    u32 sprite_textures = 0, texture_size = 0;
};

Budget default_budget(Target t);

// Budget fields by their .mceproj name, for reading, writing and listing them.
struct BudgetField {
    const char* name;
    bool integer;  // a whole number (counters, texture sizes); otherwise milliseconds
    f64 (*get)(const Budget&);
    void (*set)(Budget&, f64);
};
Span<const BudgetField> budget_fields();
const BudgetField* find_budget_field(StringView name);

struct BudgetLine {
    const char* metric = "";  // a BudgetField name
    const char* stat = "";    // "p95" or "max"
    f64 measured = 0;
    f64 limit = 0;
    bool over = false;
};

struct Verdict {
    bool pass = true;
    Array<BudgetLine> lines;  // every limited, measured metric, in budget_fields() order
};

// The summary against the budget: a line per field the budget limits and the capture measured (a
// time with no measured frames is left out: headless runs have no GPU).
Verdict judge(const Summary& s, const Budget& b);

}  // namespace mce::perf
