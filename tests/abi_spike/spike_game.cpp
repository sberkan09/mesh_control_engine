// Stand-in game.dll: the only export is the module entry point.
#include "spike_api.h"

namespace {

using namespace mce::spike;

struct Health {
    mce::i32 hp;
};

mce::u32 probe_type_id(int which) {
    return which == 0 ? type_id<Position>() : type_id<Health>();
}

mce::u32 probe_naive_id(int which) {
    return which == 0 ? naive_type_id<Position>() : naive_type_id<Health>();
}

void install(HostContext& ctx) {
    ctx.damage->push(Damage{7, -25});
    ctx.damage->push(Damage{9, -5});
}

const ModuleInfo kInfo{"abi_spike_game", &probe_type_id, &probe_naive_id, &install};

}  // namespace

MCE_SPIKE_MODULE_EXPORT const ModuleInfo* mce_game_module() { return &kInfo; }
