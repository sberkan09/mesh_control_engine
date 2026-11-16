// Scene scripts: kit::ScriptSystem components -> one ScriptHost per World. See
// mce/script/scene_scripts.h.

#include "mce/script/scene_scripts.h"

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/convar.h"
#include "mce/hash.h"
#include "mce/kit/components.h"
#include "mce/log.h"
#include "mce/type_name.h"

namespace mce::script {

namespace {

MCE_CVAR(cv_state_check, "script.state_check", -1,
         "Fail a script that keeps state between ticks (tables it reaches are frozen, upvalues and buffers watched): -1 = the host's default "
         "(on, except in a shipped game's window), 0 = off, 1 = on",
         CVarFlags::None);

struct Entry {
    i32 order = 0;
    u32 seq = 0;  // ECS order
    asset::Guid guid;
    kit::ScriptPhase phase = kit::ScriptPhase::Pre;
    bool during_cutscenes = false;
};

// The host runs one group per call. A script's group is its phase, plus kCutsceneGroup when it runs
// through cutscene holds: a cutscene-held tick runs only that second group.
constexpr u8 kCutsceneGroup = 2;

u8 group_of(const Entry& e) { return static_cast<u8>(e.phase) | (e.during_cutscenes ? kCutsceneGroup : 0); }

struct SceneScripts {
    asset::AssetSystem* assets = nullptr;
    ScriptBudget budget;
    mem::UniquePtr<ScriptHost> host;
    u64 signature = 0;  // of the loaded list; 0 = nothing loaded
    bool reported_no_assets = false;
};

u64 read_entries(Frame& f, const asset::AssetSystem* assets, Array<Entry>& out) {
    out.clear();
    // Unregistered: no scene put a ScriptSystem here. Querying would register it, and a world
    // without scripts must stay exactly as it was (same components, same hash).
    if (f.world().find_component(kTypeHash<kit::ScriptSystem>) == ecs::kInvalidComponent) return 0;
    u32 seq = 0;
    f.query<const kit::ScriptSystem&>().each([&](const kit::ScriptSystem& s) {
        const u32 at = seq++;
        if (!s.enabled) return;
        Entry e;
        e.order = s.order;
        e.seq = at;
        e.guid = s.script.guid;
        e.phase = s.phase;
        e.during_cutscenes = s.during_cutscenes;
        // Insertion keeps (order, seq): the list is a handful of scripts.
        usize i = out.size();
        out.push_back(e);
        while (i > 0 && out[i - 1].order > e.order) {
            out[i] = out[i - 1];
            --i;
        }
        out[i] = e;
    });
    if (out.empty()) return 0;
    u64 h = 0x5C121D75C121D7ull;
    for (const Entry& e : out) {
        h = mix64(h ^ e.guid.hi);
        h = mix64(h ^ e.guid.lo);
        h = mix64(h ^ (u64(static_cast<u32>(e.order)) << 8 | u64(group_of(e))));
        // A recooked script (editor hot reload bumps the asset's generation) rebuilds the host too.
        if (assets) {
            const asset::AssetKey key = assets->find(e.guid);
            if (key != asset::kInvalidAssetKey) h = mix64(h ^ assets->generation(key));
        }
    }
    return h | 1;  // never 0
}

void load(SceneScripts& s, Span<const Entry> entries) {
    s.host = mem::make_unique<ScriptHost>(mem::heap(), s.budget);
    s.host->set_assets(s.assets);
    for (const Entry& e : entries) {
        const asset::AssetKey key = s.assets->find(e.guid);
        if (key == asset::kInvalidAssetKey) {
            MCE_LOG_ERROR("script {}: not in the mounted assets (is it cooked?)", asset::to_string(e.guid).c_str());
            continue;
        }
        const asset::AssetInfo info = s.assets->info(key);
        Result<asset::AssetHandle<asset::Script>> h = s.assets->load_sync<asset::Script>(key);
        if (!h) {
            MCE_LOG_ERROR("script {}: {} does not load", asset::to_string(e.guid).c_str(), info.path);
            continue;
        }
        Result<asset::ScriptView> view = asset::ScriptView::read(s.assets->resolve(h.value()));
        if (view) {
            const Span<const u8> bc(reinterpret_cast<const u8*>(view.value().bytecode.data()),
                                    view.value().bytecode.size());
            (void)s.host->add_bytecode(view.value().name, bc, group_of(e));  // failures are logged
        } else {
            MCE_LOG_ERROR("script {}: {} is not a script chunk", asset::to_string(e.guid).c_str(), info.path);
        }
        s.assets->release(asset::AnyHandle(h.value()));
    }
}

void sync(Frame& f, SceneScripts& s) {
    Array<Entry> entries;
    const u64 sig = read_entries(f, s.assets, entries);
    if (sig == s.signature) return;
    s.signature = sig;
    s.host.reset();
    if (sig == 0) return;
    if (!s.assets) {
        if (!s.reported_no_assets) MCE_LOG_ERROR("script: this host has no assets mounted; ScriptSystems do not run");
        s.reported_no_assets = true;
        return;
    }
    load(s, entries.span());
}

void run_phase(Frame& f, kit::ScriptPhase phase) {
    SceneScripts& s = f.resource<SceneScripts>();
    sync(f, s);
    if (!s.host) return;
    const u8 p = static_cast<u8>(phase);
    if (!f.world().cutscene_held()) s.host->run(f, p);
    s.host->run(f, static_cast<u8>(p | kCutsceneGroup));
}

void script_pre(Frame& f) { run_phase(f, kit::ScriptPhase::Pre); }
void script_post(Frame& f) { run_phase(f, kit::ScriptPhase::Post); }

}  // namespace

void install_scene_scripts(World& w, asset::AssetSystem* assets, const ScriptBudget& budget) {
    if (w.try_resource<SceneScripts>()) return;
    SceneScripts& s = w.set_resource(SceneScripts{});
    s.assets = assets;
    s.budget = budget;
    if (cv_state_check.get_int() >= 0) s.budget.check_state = cv_state_check.get_int() != 0;
    w.add_system(Phase::FixedUpdate_Pre, "script.pre", &script_pre);
    w.add_system(Phase::FixedUpdate_Post, "script.post", &script_post);
    // Both run through cutscene holds, but only the scripts that ask (during_cutscenes) do any work then.
    w.run_during_cutscenes(Phase::FixedUpdate_Pre, "script.pre");
    w.run_during_cutscenes(Phase::FixedUpdate_Post, "script.post");
}

const ScriptHost* scene_script_host(World& w) {
    const SceneScripts* s = w.try_resource<SceneScripts>();
    return s ? s->host.get() : nullptr;
}

}  // namespace mce::script
