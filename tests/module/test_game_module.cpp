// The synthetic game module of the loader tests (scripting.md §6.1). Built in several variants
// from this one file:
//   MODTEST_VARIANT_GOOD         registers components + systems + a probe resource
//   MODTEST_VARIANT_NO_ENTRY     exports nothing
//   MODTEST_VARIANT_INSTALL_FAIL install() returns an error
//   (stale engine id: the GOOD variant compiled with MCE_ENGINE_ID predefined)
#include "mce/gameplay.h"
#include "module_types.h"

namespace mce::modtest {

// A component only the module reflects: its record lives in the module image.
struct ModuleOnly {
    i32 level = 1;
};
MCE_COMPONENT(ModuleOnly, level)

}  // namespace mce::modtest

namespace {

using namespace mce;
using namespace mce::modtest;

void spawn_units(Frame& f) {
    for (u32 i = 0; i < kSpawned; ++i) f.commands().spawn(Health{kStartHp}, Velocity{1.0f, 0.0f});
}

void bleed(Frame& f) {
    f.query<Health&>().each([](Health& h) { --h.hp; });
}

ResultV install(World& w) {
    // Registered in the opposite order to the host: ids must still agree (one registry).
    w.register_components<Velocity, Health>();
    Probe probe;
    probe.velocity_id = w.component_id<Velocity>();
    probe.health_id = w.component_id<Health>();
    if (const ConVar* cv = ConVar::find(StringId("test.module_host_cvar"))) probe.host_cvar = cv->get_int();
    w.set_resource(probe);
    w.resource<Probe>().installs += 1;

    w.add_system(Phase::Startup, "modtest_spawn", &spawn_units);
    w.add_system(Phase::FixedUpdate_Pre, "modtest_bleed", &bleed);
#if defined(MODTEST_VARIANT_INSTALL_FAIL)
    // Fails after registering: the World now holds this module's code pointers.
    return Error(Errc::InvalidArgument, "modtest.install", "deliberate failure");
#else
    return ok();
#endif
}

}  // namespace

#if !defined(MODTEST_VARIANT_NO_ENTRY)
MCE_GAME_MODULE("modtest", 3, &install)
#else
// Keep install referenced so the variant compiles warning-free.
MCE_MODULE_EXPORT const void* mce_not_the_entry() { return reinterpret_cast<const void*>(&install); }
#endif
