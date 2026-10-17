#include "mce/editor/perf_file.h"

#include "mce/cook/fs.h"

namespace mce::editor {

namespace {

f32 num_f(cook::JsonValue v, f32 fallback = -1.0f) { return v.is_number() ? static_cast<f32>(v.as_number()) : fallback; }
u64 num_u(cook::JsonValue v) { return v.is_number() && v.as_number() > 0 ? static_cast<u64>(v.as_number()) : 0; }

void read_names(cook::JsonValue v, usize max, Array<FixedString<32>>& out) {
    out.clear();
    for (u32 i = 0; v.is_array() && i < v.size() && out.size() < max; ++i)
        out.push_back(FixedString<32>(v.at(i).is_string() ? v.at(i).as_string() : StringView("?")));
}

// `slot` maps the position in the file's "draw_causes" array to a DrawCause index (~0u: a name this engine lacks).
void read_counters(cook::JsonValue v, perf::Counters& c, const u32* slot, u32 slots) {
    c.draws = static_cast<u32>(num_u(v.get("draws")));
    c.passes = static_cast<u32>(num_u(v.get("passes")));
    c.dispatches = static_cast<u32>(num_u(v.get("dispatches")));
    c.triangles = static_cast<u32>(num_u(v.get("triangles")));
    c.meshes = static_cast<u32>(num_u(v.get("meshes")));
    c.sprites = static_cast<u32>(num_u(v.get("sprites")));
    c.sprite_batches = static_cast<u32>(num_u(v.get("sprite_batches")));
    c.ring_overflows = static_cast<u32>(num_u(v.get("ring_overflows")));
    c.upload_bytes = num_u(v.get("upload_bytes"));
    c.heap_allocations = num_u(v.get("heap_allocations"));
    c.runs_instanced = static_cast<u32>(num_u(v.get("runs_instanced")));  // absent before M-VISUAL3 5a: 0
    c.draws_instanced = static_cast<u32>(num_u(v.get("draws_instanced")));
    const cook::JsonValue dc = v.get("draw_causes");  // absent before M-CONTENT 2: zeros
    for (u32 i = 0; dc.is_array() && i < dc.size() && i < slots; ++i)
        if (slot[i] < kDrawCauseCount) c.draw_causes[slot[i]] = static_cast<u32>(num_u(dc.at(i)));
}

}  // namespace

bool read_mcperf(StringView text, perf::Capture& out, cook::String& err) {
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!doc.parse(text, je)) {
        cook::appendf(err, "not JSON (line %u: %s)", je.line, je.message.c_str());
        return false;
    }
    const cook::JsonValue root = doc.root();
    if (!root.is_object() || !root.get("format").is_string() || root.get("format").as_string() != "mce-perf") {
        err += "not a .mcperf capture (\"format\" must be \"mce-perf\")";
        return false;
    }
    if (num_u(root.get("version")) != perf::kFormatVersion) {
        cook::appendf(err, "a .mcperf of version %llu; this engine reads version %u",
                      static_cast<unsigned long long>(num_u(root.get("version"))), perf::kFormatVersion);
        return false;
    }
    out = perf::Capture{};
    const auto str = [&](const char* key) { return root.get(key).is_string() ? root.get(key).as_string() : StringView(); };
    out.app.assign(str("app"));
    out.backend.assign(str("backend"));
    out.scene.assign(str("scene"));
    out.width = static_cast<u32>(num_u(root.get("width")));
    out.height = static_cast<u32>(num_u(root.get("height")));
    out.sim_hz = static_cast<u32>(num_u(root.get("sim_hz")));
    read_names(root.get("phases"), perf::kMaxPhases, out.phases);
    read_names(root.get("gpu_stages"), perf::kMaxGpuStages, out.gpu_stages);
    const cook::JsonValue rp = root.get("render_phases");
    for (u32 i = 0; rp.is_array() && i < rp.size(); ++i)
        for (usize p = 0; p < out.phases.size(); ++p)
            if (rp.at(i).is_string() && out.phases[p].view() == rp.at(i).as_string()) out.render_phases |= 1u << p;

    const cook::JsonValue systems = root.get("systems");
    for (u32 i = 0; systems.is_array() && i < systems.size(); ++i) {
        const cook::JsonValue s = systems.at(i);
        perf::SystemStats st;
        st.name.assign(s.get("name").is_string() ? s.get("name").as_string() : StringView("?"));
        const StringView phase = s.get("phase").is_string() ? s.get("phase").as_string() : StringView();
        for (usize p = 0; p < out.phases.size(); ++p)
            if (out.phases[p].view() == phase) st.phase = static_cast<u8>(p);
        st.runs = num_u(s.get("runs"));
        st.total_ms = s.get("total_ms").is_number() ? s.get("total_ms").as_number() : 0.0;
        st.max_frame_ms = num_f(s.get("max_frame_ms"), 0.0f);
        out.systems.push_back(st);
    }

    u32 slot[64];  // the file names its draw causes; a name this engine does not know is skipped
    u32 slots = 0;
    const cook::JsonValue names = root.get("draw_causes");
    for (u32 i = 0; names.is_array() && i < names.size() && slots < 64; ++i, ++slots) {
        slot[slots] = ~0u;
        for (u32 k = 0; k < kDrawCauseCount && names.at(i).is_string(); ++k)
            if (names.at(i).as_string() == StringView(draw_cause_name(static_cast<DrawCause>(k)))) slot[slots] = k;
    }

    const cook::JsonValue frames = root.get("frames");
    if (!frames.is_array()) {
        err += "the capture has no \"frames\" array";
        return false;
    }
    out.frames.reserve(frames.size());
    for (u32 i = 0; i < frames.size(); ++i) {
        const cook::JsonValue f = frames.at(i);
        perf::Frame fr;
        fr.index = num_u(f.get("index"));
        fr.tick = num_u(f.get("tick"));
        fr.ticks = static_cast<u32>(num_u(f.get("ticks")));
        fr.frame_ms = num_f(f.get("frame_ms"));
        fr.sim_ms = num_f(f.get("sim_ms"), 0.0f);
        fr.render_ms = num_f(f.get("render_ms"), 0.0f);
        fr.gpu_ms = num_f(f.get("gpu_ms"));
        const cook::JsonValue ph = f.get("phase_ms"), gs = f.get("gpu_stage_ms");
        for (u32 p = 0; ph.is_array() && p < ph.size() && p < perf::kMaxPhases; ++p) fr.phase_ms[p] = num_f(ph.at(p), 0.0f);
        for (u32 g = 0; gs.is_array() && g < gs.size() && g < perf::kMaxGpuStages; ++g) fr.gpu_stage_ms[g] = num_f(gs.at(g));
        read_counters(f.get("counters"), fr.counters, slot, slots);
        out.frames.push_back(fr);
    }
    return true;
}

bool load_mcperf(StringView path, perf::Capture& out, cook::String& err) {
    cook::String text;
    if (!cook::fs::read_text(path, text)) {
        err += "cannot read ";
        err += path;
        return false;
    }
    if (read_mcperf(text.view(), out, err)) return true;
    cook::String why(err.view());
    err.clear();
    err += path;
    err += ": ";
    err += why.view();
    return false;
}

}  // namespace mce::editor
