#pragma once
// MixerModel — the scene's audio mixer as data an editor can show (M-MIXEDIT): the AudioBusEffects, AudioReverb,
// AudioLimiter and AudioDuck components that drive the device (mce/audio/components.h), found across the open scene
// and written back through EditorScene, so every change is one undo entry and the scene file stays the single source.
// Like the runtime, the first entity in document order that holds a bus's (or unit's) component is the one that
// counts. A setter without such an entity creates a "Mixer <name>" entity for it; a clear removes the component and
// the entity too when it was a "Mixer ..." entity left empty. Headless, tested in CI; the Mixer panel draws it.

#include "mce/audio/components.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

class MixerModel {
public:
    explicit MixerModel(EditorScene& scene) : scene_(scene) {}

    // False: the scene has none (`out` is then the component's defaults, i.e. a dry, unfiltered bus).
    bool bus_effects(audio::AudioBus bus, audio::AudioBusEffects& out) const;
    bool reverb(u32 unit, audio::AudioReverb& out) const;
    bool limiter(audio::AudioLimiter& out) const;
    bool duck(audio::AudioBus bus, audio::AudioDuck& out) const;

    // Writes the value (its bus or unit field is forced to the one named). False when nothing changed.
    bool set_bus_effects(audio::AudioBus bus, audio::AudioBusEffects value);
    bool set_reverb(u32 unit, audio::AudioReverb value);
    bool set_limiter(const audio::AudioLimiter& value);
    bool set_duck(audio::AudioBus bus, audio::AudioDuck value);

    // Removes the component (the bus goes back to dry). False when the scene had none.
    bool clear_bus_effects(audio::AudioBus bus);
    bool clear_reverb(u32 unit);
    bool clear_limiter();
    bool clear_duck(audio::AudioBus bus);

private:
    EditorScene& scene_;
};

}  // namespace mce::editor
