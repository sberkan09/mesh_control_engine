// The Scenes panel (M-SCENETABS 2): a tab per open scene file. Opening another scene (Assets, File > Open Scene)
// parks the unsaved changes of the one that was open in its autosave file instead of asking, and a tab brings them
// back; the model is SceneTabs (mce/editor/scene_tabs.h). A dirty tab shows `*`; closing a dirty tab asks.

#include <imgui.h>

#include "mce/cook/string.h"
#include "mce/editor/editor_shell.h"
#include "mce/log.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

class ScenesPanel final : public IEditorPanel {
public:
    const char* name() const override { return kScenesPanel; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        SceneTabs& tabs = shell.scene_tabs();
        if (!session.project().is_open() || shell.playing()) {
            ImGui::TextDisabled(shell.playing() ? "The scene is locked while playing." : "Open a project to edit its scenes.");
            return;
        }
        tabs.sync(session);
        if (tabs.tabs().empty()) {
            ImGui::TextDisabled("Open a scene from Assets or File > Open Scene; each one gets a tab here.");
            return;
        }
        int want_activate = -1;
        int want_close = -1;
        if (ImGui::BeginTabBar("##scene_tabs", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
            for (u32 i = 0; i < tabs.tabs().size(); ++i) {
                const SceneTabs::Tab& t = tabs.tabs()[i];
                const StringView p = t.path.view();
                const usize slash = p.find_last_of("/\\");
                cook::String label(slash == StringView::npos ? p : p.substr(slash + 1));
                if (tabs.dirty(session, i)) label += '*';
                cook::appendf(label, "###tab%u", i);
                bool open = true;
                const bool is_active = static_cast<int>(i) == tabs.active();
                if (ImGui::BeginTabItem(label.c_str(), &open, is_active ? ImGuiTabItemFlags_SetSelected : 0)) ImGui::EndTabItem();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.path.c_str());
                if (!is_active && ImGui::IsItemClicked()) want_activate = static_cast<int>(i);
                if (!open) want_close = static_cast<int>(i);
            }
            ImGui::EndTabBar();
        }
        cook::String err;
        if (want_activate >= 0 && !tabs.activate(session, static_cast<u32>(want_activate), err)) error_ = err;
        else if (want_activate >= 0) {
            error_ = cook::String();
            shell.frame_scene();
        }
        if (want_close >= 0) {
            if (tabs.dirty(session, static_cast<u32>(want_close))) close_index_ = want_close;
            else close_tab(shell, static_cast<u32>(want_close), false);
        }
        if (close_index_ >= 0) draw_close_prompt(shell);
        if (!error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());
    }

private:
    void close_tab(EditorShell& shell, u32 index, bool discard) {
        cook::String err;
        if (shell.scene_tabs().close(shell.session(), index, discard, err)) {
            error_ = cook::String();
            shell.frame_scene();
        } else {
            error_ = err;
        }
    }

    void draw_close_prompt(EditorShell& shell) {
        const SceneTabs& tabs = shell.scene_tabs();
        if (static_cast<u32>(close_index_) >= tabs.tabs().size()) {
            close_index_ = -1;
            return;
        }
        ImGui::Text("Close %s and drop its unsaved changes?", tabs.tabs()[static_cast<u32>(close_index_)].path.c_str());
        if (ImGui::Button("Discard and close")) {
            close_tab(shell, static_cast<u32>(close_index_), true);
            close_index_ = -1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep open")) close_index_ = -1;
    }

    cook::String error_;
    int close_index_ = -1;
};

}  // namespace

void add_scenes_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<ScenesPanel>(mem::heap())); }

}  // namespace mce::editor
