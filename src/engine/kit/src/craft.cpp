#include "mce/kit/craft.h"

#include "kit3d_internal.h"
#include "mce/ecs/world.h"
#include "mce/kit/loot.h"

namespace mce::kit {
namespace {

struct CraftInstalled {};

u32 saturate_us(f32 dt) {
    const f32 us = dt * 1.0e6f + 0.5f;
    return us <= 0.0f ? 0u : (us >= 4.0e9f ? 4000000000u : static_cast<u32>(us));
}

Inventory* inventory_of(World& w, Entity e) { return e.is_null() || !w.alive(e) ? nullptr : w.try_get_mut<Inventory>(e); }

void fail(EventAccess& ev, const CraftItem& c, CraftResult r) { ev.push(CraftFailed{c.owner, c.recipe, c.station, static_cast<u32>(r)}); }

void kit_craft(Frame& f) {
    World& w = f.world();
    ItemRegistry& reg = w.resource<ItemRegistry>();
    EventAccess ev = f.events();

    // Queued crafts at stations finish first, so a request read this tick starts a full tick later.
    const u32 step_us = saturate_us(f.dt);
    f.query<CraftingStation>().each([&](Entity e, CraftingStation& st) {
        if (st.recipe.empty()) return;
        st.left_us = st.left_us > step_us ? st.left_us - step_us : 0u;
        if (st.left_us != 0) return;
        const Entity owner = st.crafter;
        const FixedString<64> id = st.recipe;
        st.recipe.clear();
        st.crafter = Entity();
        Inventory* inv = inventory_of(w, owner);
        const asset::ItemCatalog* c = inv ? reg.resolve(inv->catalog) : nullptr;
        CraftResult r = CraftResult::NoInventory;
        if (c) {
            const u16 recipe = c->find_recipe(id.view());
            if (recipe == asset::kNoItem) r = CraftResult::NoRecipe;
            else if (!c->recipes()[recipe].station.empty() && st.station.view() != c->recipes()[recipe].station.view()) r = CraftResult::NoStation;  // retagged or reloaded meanwhile
            else r = craft_apply(*inv, *c, recipe);
        }
        if (r == CraftResult::Ok) ev.push(ItemCrafted{owner, id, e});
        else ev.push(CraftFailed{owner, id, e, static_cast<u32>(r)});
    });

    for (const CraftItem& q : ev.read<CraftItem>()) {
        Inventory* inv = inventory_of(w, q.owner);
        const asset::ItemCatalog* c = inv ? reg.resolve(inv->catalog) : nullptr;
        if (!c) {
            fail(ev, q, CraftResult::NoInventory);
            continue;
        }
        const u16 recipe = c->find_recipe(q.recipe.view());
        if (recipe == asset::kNoItem) {
            fail(ev, q, CraftResult::NoRecipe);
            continue;
        }
        const asset::Recipe& rc = c->recipes()[recipe];
        CraftingStation* st = q.station.is_null() || !w.alive(q.station) ? nullptr : w.try_get_mut<CraftingStation>(q.station);
        if (!rc.station.empty() && (!st || st->station.view() != rc.station.view())) {
            fail(ev, q, CraftResult::NoStation);
            continue;
        }
        if (rc.time_ms == 0 || rc.station.empty() || !st) {  // instant, or by hand
            const CraftResult r = craft_apply(*inv, *c, recipe);
            if (r == CraftResult::Ok) ev.push(ItemCrafted{q.owner, q.recipe, q.station});
            else fail(ev, q, r);
            continue;
        }
        if (!st->recipe.empty()) {
            fail(ev, q, CraftResult::StationBusy);
            continue;
        }
        const CraftResult r = craft_check(*inv, *c, recipe);
        if (r != CraftResult::Ok) {
            fail(ev, q, r);
            continue;
        }
        st->recipe = q.recipe;
        st->crafter = q.owner;
        const u64 us = u64(rc.time_ms) * 1000u;
        st->left_us = us > 4000000000ull ? 4000000000u : static_cast<u32>(us);
    }
}

}  // namespace

const char* craft_result_name(CraftResult r) {
    switch (r) {
        case CraftResult::Ok: return "ok";
        case CraftResult::NoRecipe: return "no_recipe";
        case CraftResult::MissingInput: return "missing_input";
        case CraftResult::NoRoom: return "no_room";
        case CraftResult::NoStation: return "no_station";
        case CraftResult::StationBusy: return "station_busy";
        case CraftResult::NoInventory: return "no_inventory";
    }
    return "";
}

CraftResult craft_check(const Inventory& inv, const asset::ItemCatalog& c, u32 recipe) {
    Inventory trial = inv;
    return craft_apply(trial, c, recipe);
}

CraftResult craft_apply(Inventory& inv, const asset::ItemCatalog& c, u32 recipe) {
    if (recipe >= c.recipes().size()) return CraftResult::NoRecipe;
    const asset::Recipe& r = c.recipes()[recipe];
    const Span<const asset::RecipeIngredient> in = c.inputs_of(r);
    const Span<const asset::RecipeIngredient> out = c.outputs_of(r);
    for (const asset::RecipeIngredient& i : in)  // every input first, so a shortage is reported before room is
        if (i.item >= c.items().size() || inventory_count(inv, i.item) < i.qty) return CraftResult::MissingInput;
    Inventory trial = inv;  // the whole craft happens on a copy and is committed only when it all fit
    for (const asset::RecipeIngredient& i : in)
        if (inventory_remove(trial, c, i.item, i.qty) != i.qty) return CraftResult::MissingInput;
    for (const asset::RecipeIngredient& o : out)
        if (o.item >= c.items().size() || inventory_add(trial, c, o.item, o.qty) != o.qty) return CraftResult::NoRoom;
    inv = trial;
    return CraftResult::Ok;
}

void install_crafting(World& world, asset::AssetSystem* assets) {
    install_loot(world, assets);
    if (world.try_resource<CraftInstalled>()) return;
    world.set_resource(CraftInstalled{});
    world.register_components<CraftingStation>();
    world.register_event<CraftItem>(Phase::FixedUpdate_Pre);
    world.register_event<ItemCrafted>(Phase::FixedUpdate_Pre);
    world.register_event<CraftFailed>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_craft", &kit_craft, "kit_loot");
}

}  // namespace mce::kit
