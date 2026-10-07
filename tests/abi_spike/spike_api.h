#pragma once
// game.dll ABI spike (ROADMAP M1, finding F33). Throwaway: proves the cross-DLL template
// rules with a stand-in engine DLL before the real mce_engine umbrella DLL exists (M3).
//
// Rules exercised (scripting.md §3.1, finding F11):
//   1. Templated engine APIs reachable from a module ship as explicit, exported
//      instantiations in the engine DLL (`extern template` + MCE_SPIKE_API).
//   2. Engine header templates hold no function-local statics. Type identity is the
//      constexpr kTypeHash<T>, mapped to a dense id by an exported non-template function
//      whose state lives once, in the engine DLL.

#include "mce/type_name.h"

#if defined(_WIN32)
#if defined(MCE_SPIKE_ENGINE_BUILD)
#define MCE_SPIKE_API __declspec(dllexport)
#else
#define MCE_SPIKE_API __declspec(dllimport)
#endif
#define MCE_SPIKE_MODULE_EXPORT extern "C" __declspec(dllexport)
// MSVC wants the export on the explicit instantiation definition too.
#define MCE_SPIKE_INSTANTIATION_API MCE_SPIKE_API
#else
#define MCE_SPIKE_API __attribute__((visibility("default")))
#define MCE_SPIKE_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
// GCC/Clang take visibility from the `extern template` declaration and reject it here.
#define MCE_SPIKE_INSTANTIATION_API
#endif

namespace mce::spike {

// ---- rule 2: identity through an exported non-template registry -------------------------
MCE_SPIKE_API u32 type_id_from_hash(u64 type_hash);
MCE_SPIKE_API u32 registered_type_count();

template <class T>
u32 type_id() {
    return type_id_from_hash(kTypeHash<T>);  // no static here: the registry owns the state
}

// The trap rule 2 forbids, kept for the demonstration only: each image that instantiates
// this gets its own `id` on Windows (COMDAT folding stops at the DLL edge).
MCE_SPIKE_API u32 next_naive_id();
template <class T>
u32 naive_type_id() {
    static const u32 id = next_naive_id();
    return id;
}

// ---- rule 1: explicit exported instantiation ---------------------------------------------
// Member functions are defined only in the engine DLL, so a module links only if the
// exported instantiation for its argument exists.
template <class T>
class EventLog {
public:
    void push(const T& e);
    u32 count() const;
    T last() const;

private:
    T items_[16]{};
    u32 count_ = 0;
};

struct Damage {
    u32 target;
    i32 amount;
};

extern template class MCE_SPIKE_API EventLog<Damage>;

// What the host hands the module: engine-owned state the module mutates through exported code.
struct HostContext {
    EventLog<Damage>* damage;
};

struct ModuleInfo {
    const char* name;
    // Ids the module computed for the probe types, compared against the host's.
    u32 (*probe_type_id)(int which);
    u32 (*probe_naive_id)(int which);
    void (*install)(HostContext& ctx);
};

using ModuleEntry = const ModuleInfo* (*)();

// A component type both sides name; the module defines more of its own.
struct Position {
    f32 x, y;
};

}  // namespace mce::spike
