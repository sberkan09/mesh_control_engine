// systems_tour: M-COVER's code-side tour, headless. Three systems a scene file cannot start on its own
// show up as components on one NPC:
//   NetReplicated / NetOwner   the NPC is a replicated entity of archetype 1 owned by peer 0 (the
//                              ReplicationServer of samples/net_headless would mirror it to clients)
//   Dialogue / DialogueInput   a conversation from a story `mce story compile` built from story/, driven by
//                              the stock confirm key
// It runs the conversation to its end and checks the story variables the player's choice set. Exit code 0
// on success.
#include <cstdio>
#include <cstring>

#include "mce/core.h"
#include "mce/ecs/replicated.h"
#include "mce/ecs/world.h"
#include "mce/input/ecs.h"
#include "mce/kit/novel.h"
#include "systems_tour_story.h"

using namespace mce;
namespace nv = mce::novel;
namespace st = systems_tour;

namespace {

u64 g_tick = 0;

void step(World& w, bool confirm, f32 move_y) {
    input::InputState in;
    in.confirm_pressed = confirm;
    in.move.y = move_y;
    in.tick = g_tick;
    input::publish(w, in);
    w.tick(1.0 / 60.0, g_tick++);
}

bool fail(const char* what) {
    std::fprintf(stderr, "systems_tour: FAILED: %s\n", what);
    return false;
}

bool run(u32 ticks) {
    World w;
    input::install(w);
    if (!nv::install_dialogue(w, st::kProgram)) return fail("install_dialogue");

    const Entity npc = w.spawn();
    NetReplicated rep;
    rep.kind = 1;
    NetOwner owner;
    owner.peer = 0;
    w.add(npc, rep);
    w.add(npc, owner);
    w.add(npc, nv::Dialogue{});
    w.add(npc, nv::DialogueInput{});
    step(w, false, 0.f);
    step(w, false, 0.f);

    if (!nv::start_dialogue(w, npc, "guide")) return fail("start_dialogue");
    bool picked = false;
    for (u32 i = 0; i < ticks; ++i) {
        const nv::Dialogue* d = w.try_get<nv::Dialogue>(npc);
        if (!d->active) break;
        // Enter shows the page, then advances; at the choice the first option ("What runs on the server?") is picked.
        step(w, true, 0.f);
        step(w, false, 0.f);
        picked = picked || d->waiting_choice;
    }
    const nv::Dialogue* d = w.try_get<nv::Dialogue>(npc);
    if (d->active) return fail("the conversation did not reach its end");
    if (!picked) return fail("the conversation never waited on a choice");
    const nv::StoryState* vars = nv::dialogue_vars(w);
    if (!vars) return fail("no story variables");
    if (nv::get_var(*vars, st::var::talks) != 1) return fail("talks != 1");
    if (nv::get_var(*vars, st::var::asked_server) != 1) return fail("asked_server not set");
    if (w.try_get<NetReplicated>(npc)->kind != 1 || w.try_get<NetOwner>(npc)->peer != 0) return fail("net components");
    std::printf("systems_tour: dialogue ended after %llu ticks, talks %d, asked_server %d\n",
                static_cast<unsigned long long>(g_tick), nv::get_var(*vars, st::var::talks),
                nv::get_var(*vars, st::var::asked_server));
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    u32 ticks = 200;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "-ticks")) ticks = static_cast<u32>(std::atoi(argv[i + 1]));
    const bool ok = run(ticks);
    std::printf("systems_tour: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
