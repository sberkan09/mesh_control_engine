#include "mce/editor/mixer_model.h"

#include "mce/cook/string.h"

namespace mce::editor {
namespace {

using audio::AudioBus;

// The first component of type T in document order for which `match(value)` holds; `owner` is its entity.
template <class T, class Match>
const T* find_first(const EditorScene& scene, Match match, EntityId& owner) {
    const refl::TypeRecord& type = refl::type_of<T>();
    for (const DocEntity& e : scene.doc().entities()) {
        const DocComponent* c = e.find(type);
        if (!c) continue;
        const T* v = static_cast<const T*>(c->data());
        if (!match(*v)) continue;
        owner = e.id;
        return v;
    }
    return nullptr;
}

const char* bus_name(AudioBus bus) {
    switch (bus) {
    case AudioBus::Master: return "Master";
    case AudioBus::Music: return "Music";
    case AudioBus::SFX: return "SFX";
    case AudioBus::UI: return "UI";
    case AudioBus::Voice: return "Voice";
    }
    return "Bus";
}

// Writes `value` onto the entity that holds the matching component, or onto a new "Mixer <label>" entity.
template <class T, class Match>
bool put(EditorScene& scene, Match match, const char* label, const T& value) {
    const refl::TypeRecord& type = refl::type_of<T>();
    EntityId owner = kNoEntity;
    const T* have = find_first<T>(scene, match, owner);
    scene.begin_edit("Edit the mixer");
    if (!have) {
        cook::String name = "Mixer ";
        name += label;
        owner = scene.create_entity(name.view(), false);
        scene.add_component(owner, type);
    }
    const bool changed = scene.set_component(owner, type, &value);
    scene.end_edit();
    return changed || !have;  // set_component is a no-op on an unchanged value
}

// Removes the matching component; an entity named "Mixer ..." left without components goes with it.
template <class T, class Match>
bool clear(EditorScene& scene, Match match) {
    const refl::TypeRecord& type = refl::type_of<T>();
    EntityId owner = kNoEntity;
    if (!find_first<T>(scene, match, owner)) return false;
    scene.begin_edit("Edit the mixer");
    scene.remove_component(owner, type);
    if (const DocEntity* e = scene.doc().find(owner); e && e->components.empty() && e->name.view().substr(0, 5) == "Mixer")
        scene.destroy_entity(owner);
    scene.end_edit();
    return true;
}

}  // namespace

bool MixerModel::bus_effects(AudioBus bus, audio::AudioBusEffects& out) const {
    EntityId owner;
    const audio::AudioBusEffects* v = find_first<audio::AudioBusEffects>(scene_, [&](const auto& x) { return x.bus == bus; }, owner);
    out = v ? *v : audio::AudioBusEffects();
    out.bus = bus;
    return v != nullptr;
}

bool MixerModel::reverb(u32 unit, audio::AudioReverb& out) const {
    EntityId owner;
    const audio::AudioReverb* v = find_first<audio::AudioReverb>(scene_, [&](const auto& x) { return x.unit == unit; }, owner);
    out = v ? *v : audio::AudioReverb();
    out.unit = static_cast<u8>(unit);
    return v != nullptr;
}

bool MixerModel::limiter(audio::AudioLimiter& out) const {
    EntityId owner;
    const audio::AudioLimiter* v = find_first<audio::AudioLimiter>(scene_, [](const auto&) { return true; }, owner);
    out = v ? *v : audio::AudioLimiter();
    return v != nullptr;
}

bool MixerModel::duck(AudioBus bus, audio::AudioDuck& out) const {
    EntityId owner;
    const audio::AudioDuck* v = find_first<audio::AudioDuck>(scene_, [&](const auto& x) { return x.bus == bus; }, owner);
    out = v ? *v : audio::AudioDuck();
    out.bus = bus;
    return v != nullptr;
}

bool MixerModel::set_bus_effects(AudioBus bus, audio::AudioBusEffects value) {
    value.bus = bus;
    return put<audio::AudioBusEffects>(scene_, [&](const auto& x) { return x.bus == bus; }, bus_name(bus), value);
}

bool MixerModel::set_reverb(u32 unit, audio::AudioReverb value) {
    value.unit = static_cast<u8>(unit);
    return put<audio::AudioReverb>(scene_, [&](const auto& x) { return x.unit == unit; }, unit ? "Reverb 1" : "Reverb 0", value);
}

bool MixerModel::set_limiter(const audio::AudioLimiter& value) {
    return put<audio::AudioLimiter>(scene_, [](const auto&) { return true; }, "Limiter", value);
}

bool MixerModel::set_duck(AudioBus bus, audio::AudioDuck value) {
    value.bus = bus;
    cook::String label = "Duck ";
    label += bus_name(bus);
    return put<audio::AudioDuck>(scene_, [&](const auto& x) { return x.bus == bus; }, label.c_str(), value);
}

bool MixerModel::clear_bus_effects(AudioBus bus) {
    return clear<audio::AudioBusEffects>(scene_, [&](const auto& x) { return x.bus == bus; });
}

bool MixerModel::clear_reverb(u32 unit) {
    return clear<audio::AudioReverb>(scene_, [&](const auto& x) { return x.unit == unit; });
}

bool MixerModel::clear_limiter() {
    return clear<audio::AudioLimiter>(scene_, [](const auto&) { return true; });
}

bool MixerModel::clear_duck(AudioBus bus) {
    return clear<audio::AudioDuck>(scene_, [&](const auto& x) { return x.bus == bus; });
}

}  // namespace mce::editor
