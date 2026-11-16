// The Mixer panel (M-MIXEDIT 2): the open scene's audio mixer as sections: one strip per bus (low-pass, high-pass and
// the reverb send), the two reverb units, the master limiter and the ducks. It edits the AudioBusEffects, AudioReverb,
// AudioLimiter and AudioDuck components through MixerModel (mce/editor/mixer_model.h), so the scene file stays the one
// source, a slider drag is one undo entry and nothing moves the sim hash. "On" adds the component, turning it off
// removes it (the bus goes back to dry).

#include <imgui.h>

#include "mce/editor/editor_shell.h"
#include "mce/editor/mixer_model.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

using audio::AudioBus;

constexpr AudioBus kBuses[] = {AudioBus::Master, AudioBus::Music, AudioBus::SFX, AudioBus::UI, AudioBus::Voice};
constexpr AudioBus kDucked[] = {AudioBus::Music, AudioBus::SFX, AudioBus::UI};
constexpr const char* kBusNames[] = {"Master", "Music", "SFX", "UI", "Voice"};

class MixerPanel final : public IEditorPanel {
public:
    const char* name() const override { return kMixerPanel; }

    void draw(EditorShell& shell) override {
        EditorScene& scene = shell.session().scene();
        MixerModel mix(scene);
        ImGui::TextDisabled("Edits the mixer components of the open scene (undoable). Off = the default.");
        if (ImGui::CollapsingHeader("Buses", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const AudioBus bus : kBuses) draw_bus(scene, mix, bus);
        }
        if (ImGui::CollapsingHeader("Reverb units", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (u32 unit = 0; unit < 2; ++unit) draw_reverb(scene, mix, unit);
        }
        if (ImGui::CollapsingHeader("Limiter", ImGuiTreeNodeFlags_DefaultOpen)) draw_limiter(scene, mix);
        if (ImGui::CollapsingHeader("Ducking", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const AudioBus bus : kDucked) draw_duck(scene, mix, bus);
        }
    }

private:
    // A slider: writes while it moves (the model merges the writes into one undo entry) and seals the entry on release.
    static bool slider(EditorScene& scene, const char* label, f32* v, f32 lo, f32 hi, const char* fmt = "%.2f") {
        const bool changed = ImGui::SliderFloat(label, v, lo, hi, fmt);
        if (ImGui::IsItemDeactivatedAfterEdit()) scene.seal_undo();
        return changed;
    }

    static bool enabled_box(const char* label, bool on, bool& turned_on, bool& turned_off) {
        bool v = on;
        if (ImGui::Checkbox(label, &v)) {
            turned_on = v;
            turned_off = !v;
        }
        return v;
    }

    void draw_bus(EditorScene& scene, MixerModel& mix, AudioBus bus) {
        const u32 i = static_cast<u32>(bus);
        ImGui::PushID(static_cast<int>(i));
        audio::AudioBusEffects fx;
        const bool has = mix.bus_effects(bus, fx);
        bool on_now = false, off_now = false;
        enabled_box(kBusNames[i], has, on_now, off_now);
        if (on_now) mix.set_bus_effects(bus, fx);
        if (off_now) mix.clear_bus_effects(bus);
        if (has) {
            ImGui::Indent();
            bool c = slider(scene, "Low-pass (Hz)", &fx.lowpass_hz, 0.0f, 20000.0f, "%.0f");
            c |= slider(scene, "High-pass (Hz)", &fx.highpass_hz, 0.0f, 20000.0f, "%.0f");
            if (bus != AudioBus::Master) {
                int unit = fx.reverb_unit;
                if (ImGui::SliderInt("Reverb unit", &unit, 0, 1)) {
                    fx.reverb_unit = static_cast<u8>(unit);
                    c = true;
                }
                c |= slider(scene, "Reverb send (dB)", &fx.reverb_send_db, -96.0f, 12.0f, "%.1f");
            }
            if (c) mix.set_bus_effects(bus, fx);
            ImGui::Unindent();
        }
        ImGui::PopID();
    }

    void draw_reverb(EditorScene& scene, MixerModel& mix, u32 unit) {
        ImGui::PushID(static_cast<int>(unit) + 100);
        audio::AudioReverb rv;
        const bool has = mix.reverb(unit, rv);
        bool on_now = false, off_now = false;
        enabled_box(unit ? "Reverb 1" : "Reverb 0", has, on_now, off_now);
        if (on_now) mix.set_reverb(unit, rv);
        if (off_now) mix.clear_reverb(unit);
        if (has) {
            ImGui::Indent();
            bool c = slider(scene, "Room size", &rv.room_size, 0.0f, 1.0f);
            c |= slider(scene, "Damping", &rv.damping, 0.0f, 1.0f);
            c |= slider(scene, "Width", &rv.width, 0.0f, 1.0f);
            c |= slider(scene, "Return (dB)", &rv.wet_db, -96.0f, 12.0f, "%.1f");
            if (c) mix.set_reverb(unit, rv);
            ImGui::Unindent();
        }
        ImGui::PopID();
    }

    void draw_limiter(EditorScene& scene, MixerModel& mix) {
        ImGui::PushID("limiter");
        audio::AudioLimiter lim;
        const bool has = mix.limiter(lim);
        bool on_now = false, off_now = false;
        enabled_box("Master limiter", has, on_now, off_now);
        if (on_now) mix.set_limiter(lim);
        if (off_now) mix.clear_limiter();
        if (has) {
            ImGui::Indent();
            bool c = slider(scene, "Ceiling (dB)", &lim.ceiling_db, -24.0f, 0.0f, "%.1f");
            c |= slider(scene, "Release (ms)", &lim.release_ms, 1.0f, 1000.0f, "%.0f");
            if (c) mix.set_limiter(lim);
            ImGui::Unindent();
        }
        ImGui::PopID();
    }

    void draw_duck(EditorScene& scene, MixerModel& mix, AudioBus bus) {
        const u32 i = static_cast<u32>(bus);
        ImGui::PushID(static_cast<int>(i) + 200);
        audio::AudioDuck d;
        const bool has = mix.duck(bus, d);
        bool on_now = false, off_now = false;
        cook::String label = kBusNames[i];
        label += " ducks";
        enabled_box(label.c_str(), has, on_now, off_now);
        if (on_now) mix.set_duck(bus, d);
        if (off_now) mix.clear_duck(bus);
        if (has) {
            ImGui::Indent();
            int key = d.key == AudioBus::Master ? 0 : static_cast<int>(d.key) - 1;
            if (ImGui::Combo("Under", &key, kBusNames + 1, 4)) {  // every bus but Master can be the key
                d.key = static_cast<AudioBus>(key + 1);
                mix.set_duck(bus, d);
            }
            bool c = slider(scene, "Depth (dB)", &d.depth_db, -60.0f, 0.0f, "%.1f");
            c |= slider(scene, "Threshold (dB)", &d.threshold_db, -96.0f, 0.0f, "%.1f");
            c |= slider(scene, "Attack (ms)", &d.attack_ms, 0.0f, 2000.0f, "%.0f");
            c |= slider(scene, "Release (ms)", &d.release_ms, 0.0f, 5000.0f, "%.0f");
            if (c) mix.set_duck(bus, d);
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
};

}  // namespace

void add_mixer_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<MixerPanel>(mem::heap())); }

}  // namespace mce::editor
