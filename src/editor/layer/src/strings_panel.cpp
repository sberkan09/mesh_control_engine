// The Strings panel (M-STRINGSED 2): every locale's string table as one grid, a row per key and a column per locale,
// the base locale ("en") first. Cells are edited in place; a locale lacking a key shows an empty cell, and "Missing
// only" lists the keys some locale still lacks. Save cooks each changed table first (mce/editor/strings_grid.h), so a
// table the cook refuses (a bad key, a duplicate) is not written and the panel shows why; Revert reads the files again.
// A double click on a string table in Assets opens the panel.

#include <imgui.h>

#include <cfloat>
#include <cstdio>
#include <cstring>

#include "mce/editor/editor_shell.h"
#include "mce/editor/strings_grid.h"
#include "mce/log.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

class StringsPanel final : public IEditorPanel {
public:
    const char* name() const override { return kStringsPanel; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        if (!session.project().is_open()) {
            ImGui::TextDisabled("Open a project to edit its strings.");
            loaded_root_ = cook::String();
            return;
        }
        if (loaded_root_.view() != session.project().root()) reload(session);
        if (grid_.locales().empty()) {
            ImGui::TextDisabled("No string tables: add assets/loc/<locale>.csv (for example en.csv) to the project.");
            if (!error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());
            if (ImGui::Button("Reload")) reload(session);
            return;
        }
        if (grid_.dirty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "(unsaved)");
            ImGui::SameLine();
        }
        if (ImGui::Button("Save")) do_save(session);
        ImGui::SameLine();
        if (ImGui::Button("Revert")) reload(session);
        if (!error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());
        draw_summary();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("Filter", filter_, sizeof(filter_));
        ImGui::SameLine();
        ImGui::Checkbox("Missing only", &missing_only_);
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("New key", new_key_, sizeof(new_key_));
        ImGui::SameLine();
        if (ImGui::Button("Add key")) {
            if (grid_.add_key(StringView(new_key_))) {
                new_key_[0] = '\0';
                error_ = cook::String();
            } else {
                error_ = cook::String("that key is empty or already exists");
            }
        }
        ImGui::Separator();
        draw_grid();
    }

private:
    void reload(EditorSession& session) {
        cook::String err;
        grid_ = StringsGrid();
        if (!grid_.load(session.project(), err)) {
            error_ = err;
        } else {
            error_ = cook::String();
        }
        loaded_root_ = cook::String(session.project().root());
    }

    void do_save(EditorSession& session) {
        cook::String err;
        if (grid_.save(session.project(), err)) {
            error_ = cook::String();
            MCE_LOG_INFO("editor: saved the string tables");
        } else {
            error_ = err;
        }
    }

    void draw_summary() {
        for (u32 l = 0; l < grid_.locales().size(); ++l) {
            const StringsGrid::Locale& loc = grid_.locales()[l];
            if (l) ImGui::SameLine();
            const u32 m = grid_.missing(l);
            if (m) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s: %u missing", loc.tag.c_str(), m);
            else ImGui::Text("%s: complete", loc.tag.c_str());
        }
    }

    bool row_visible(u32 key) const {
        const StringView k = grid_.keys()[key].view();
        if (filter_[0] && k.find(StringView(filter_)) == StringView::npos) return false;
        if (!missing_only_) return true;
        for (u32 l = 0; l < grid_.locales().size(); ++l)
            if (!grid_.has(l, key)) return true;
        return false;
    }

    void draw_grid() {
        const int cols = static_cast<int>(grid_.locales().size()) + 2;
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##strings", cols, flags)) return;
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Key");
        for (const StringsGrid::Locale& loc : grid_.locales()) ImGui::TableSetupColumn(loc.tag.c_str());
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableHeadersRow();
        i64 remove = -1;
        for (u32 key = 0; key < grid_.keys().size(); ++key) {
            if (!row_visible(key)) continue;
            ImGui::PushID(static_cast<int>(key));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(grid_.keys()[key].c_str());
            for (u32 l = 0; l < grid_.locales().size(); ++l) {
                ImGui::TableSetColumnIndex(static_cast<int>(l) + 1);
                ImGui::PushID(static_cast<int>(l));
                char buf[512];
                std::snprintf(buf, sizeof(buf), "%.*s", static_cast<int>(grid_.text(l, key).size()), grid_.text(l, key).data());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (!grid_.has(l, key)) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f, 0.30f, 0.05f, 0.5f));
                const bool edited = ImGui::InputText("##cell", buf, sizeof(buf));
                if (!grid_.has(l, key)) ImGui::PopStyleColor();
                if (edited) grid_.set_text(l, key, StringView(buf));
                ImGui::PopID();
            }
            ImGui::TableSetColumnIndex(cols - 1);
            if (ImGui::SmallButton("x")) remove = static_cast<i64>(key);
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (remove >= 0) grid_.remove_key(static_cast<u32>(remove));
    }

    StringsGrid grid_;
    cook::String loaded_root_;
    cook::String error_;
    char filter_[64] = {};
    char new_key_[96] = {};
    bool missing_only_ = false;
};

}  // namespace

void add_strings_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<StringsPanel>(mem::heap())); }

}  // namespace mce::editor
