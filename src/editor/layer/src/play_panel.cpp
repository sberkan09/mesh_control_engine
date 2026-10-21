// The Play panel (FR-EDT-060, editor-tools.md §2.8): play-in-editor controls (play, pause, single
// step, slow motion, stop), the run's numbers, the live values of the selected entity in the play
// world, and the event-flow monitor. Read-only: the play world is inspected, never edited here.

#include <imgui.h>

#include <cstdio>

#include "mce/cook/scene_text.h"
#include "mce/editor/editor_shell.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

class PlayPanel final : public IEditorPanel {
public:
    const char* name() const override { return "Play"; }

    void draw(EditorShell& shell) override {
        if (!shell.scene_mode()) {
            ImGui::TextDisabled("Open a project to play a scene.");
            return;
        }
        PlaySession& play = shell.session().play();
        draw_controls(shell, play);
        if (!play.active()) {
            const cook::DiagList& diags = shell.session().play_diags();
            for (const cook::Diagnostic& d : diags.all()) {
                cook::String line;
                cook::format_diagnostic(d, line);
                ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", line.c_str());
            }
            if (diags.all().empty())
                ImGui::TextDisabled("Play (F5) runs the scene here as it is now, saved or not; Stop brings the editor back.");
            return;
        }
        ImGui::Separator();
        const WorldMetrics m = play.world()->metrics();
        ImGui::Text("tick %llu   hash %016llx   entities %u   tables %u", static_cast<unsigned long long>(play.ticks()),
                    static_cast<unsigned long long>(play.state_hash()), m.entities, m.tables);
        ImGui::TextDisabled("entered in %.1f ms (budget 1000 ms); first-tick hash %016llx", play.enter_seconds() * 1000.0,
                            static_cast<unsigned long long>(play.enter_hash()));
        if (ImGui::BeginTable("##split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextColumn();
            draw_entity(shell, play);
            ImGui::TableNextColumn();
            draw_events(play);
            ImGui::EndTable();
        }
    }

private:
    static void draw_controls(EditorShell& shell, PlaySession& play) {
        if (!play.active()) {
            if (ImGui::Button("Play (F5)")) shell.play_in_editor();
        } else {
            const bool paused = play.state() == PlaySession::State::Paused;
            if (ImGui::Button(paused ? "Resume (F6)" : "Pause (F6)")) paused ? play.resume() : play.pause();
            ImGui::SameLine();
            if (ImGui::Button("Step (F10)")) play.step();
            ImGui::SameLine();
            if (ImGui::Button("Stop (F5)")) shell.stop_play();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        f32 scale = play.time_scale();
        if (ImGui::SliderFloat("speed", &scale, 0.05f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic)) play.set_time_scale(scale);
        static constexpr f32 kPresets[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f};
        for (f32 p : kPresets) {
            ImGui::SameLine();
            char label[16];
            std::snprintf(label, sizeof(label), "%gx", static_cast<double>(p));
            if (ImGui::SmallButton(label)) play.set_time_scale(p);
        }
    }

    // The selected scene entity's components as they are in the play world, right now.
    static void draw_entity(EditorShell& shell, PlaySession& play) {
        EditorScene& scene = shell.scene();
        const EntityId id = scene.selection().anchor();
        const i32 index = scene.doc().index_of(id);
        if (index < 0 || static_cast<usize>(index) >= play.entities().size()) {
            ImGui::TextDisabled("Select an entity in the Hierarchy to watch it live.");
            return;
        }
        const DocEntity& d = scene.doc().at(static_cast<usize>(index));
        const Entity e = play.entities()[static_cast<usize>(index)];
        World& world = *play.world();
        ImGui::Text("%s", d.name.view().empty() ? "(unnamed)" : d.name.c_str());
        if (!world.alive(e)) {
            ImGui::TextDisabled("destroyed during play");
            return;
        }
        struct Ctx {
            Span<const Entity> entities;
        } ctx{play.entities()};
        for (const DocComponent& c : d.components) {
            const ecs::ComponentId cid = world.find_component(c.type->type_hash);
            const void* live = cid == ecs::kInvalidComponent ? nullptr : world.get_raw(e, cid);
            if (!live) {
                ImGui::TextDisabled("%s: removed", c.type->name);
                continue;
            }
            cook::String text;
            cook::write_scene_component(*c.type, live, text, [](const void* field, void* user) -> u32 {
                const Entity f = *static_cast<const Entity*>(field);
                const Span<const Entity> all = static_cast<Ctx*>(user)->entities;
                for (usize i = 0; i < all.size(); ++i)
                    if (all[i] == f) return static_cast<u32>(i);
                return cook::kNoSceneEntity;
            }, &ctx);
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", c.type->name);
            ImGui::TextWrapped("%s", text.c_str());
        }
    }

    // Event-flow monitor (FR-ENT-030): every event type with its live and total counts.
    void draw_events(PlaySession& play) {
        events_.clear();
        play.world()->event_registry(events_);
        if (events_.empty()) {
            ImGui::TextDisabled("No event types registered.");
            return;
        }
        if (!ImGui::BeginTable("##events", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
        ImGui::TableSetupColumn("event");
        ImGui::TableSetupColumn("readable");
        ImGui::TableSetupColumn("total");
        ImGui::TableHeadersRow();
        for (const EventInfo& ev : events_) {
            ImGui::TableNextColumn();
            ImGui::Text("%.*s", static_cast<int>(ev.name.size()), ev.name.data());
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(ev.readable));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(ev.total_pushed));
        }
        ImGui::EndTable();
    }

    Array<EventInfo> events_;
};

}  // namespace

void add_play_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<PlayPanel>(mem::heap())); }

}  // namespace mce::editor
