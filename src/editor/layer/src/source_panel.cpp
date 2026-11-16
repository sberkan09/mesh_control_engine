// The Source panel (M-SOURCEEDIT 2): any JSON source asset as text. It opens from a double click on a kind that has
// no panel of its own (sound sets, input maps, catalogs, remote config, ...) and from "Edit source..." in the Assets
// context menu. The cook checks the text a moment after the last keystroke and the problems are listed with their
// line and column; Save cooks first (mce/editor/source_doc.h), so a document the cook refuses is not written.

#include <imgui.h>

#include <cstdio>

#include "mce/editor/editor_shell.h"
#include "mce/editor/source_doc.h"
#include "mce/log.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

class SourcePanel;
SourcePanel* g_panel = nullptr;

class SourcePanel final : public IEditorPanel {
public:
    SourcePanel() { g_panel = this; }
    ~SourcePanel() override {
        if (g_panel == this) g_panel = nullptr;
    }
    const char* name() const override { return kSourcePanel; }

    void open_asset(EditorShell& shell, asset::Guid guid) {
        const EditorProject& project = shell.session().project();
        const ProjectAsset* a = project.find(guid);
        if (!a) return;
        cook::String err;
        if (doc_.open(project, *a, err)) {
            error_ = cook::String();
            load_buffer();
            doc_.check(project);
        } else {
            error_ = err;
        }
        open = true;
        shell.focus_panel(kSourcePanel);
    }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        if (!session.project().is_open()) {
            ImGui::TextDisabled("Open a project to edit its assets.");
            return;
        }
        if (!doc_.is_open()) {
            ImGui::TextDisabled("Double-click a JSON asset in Assets, or use Edit source... in its menu.");
            if (!error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());
            return;
        }
        ImGui::TextUnformatted(doc_.path().data(), doc_.path().data() + doc_.path().size());
        if (doc_.dirty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "(unsaved)");
        }
        if (ImGui::Button("Save")) do_save(session);
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            cook::String err;
            if (doc_.revert(session.project(), err)) {
                error_ = cook::String();
                load_buffer();
                doc_.check(session.project());
            } else {
                error_ = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Check")) doc_.check(session.project());
        if (!error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());
        draw_problems();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (ImGui::InputTextMultiline("##source", buf_.data(), buf_.size(), ImVec2(avail.x, avail.y),
                                      ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize, &resize_cb, &buf_)) {
            doc_.set_text(StringView(buf_.data()));
            edited_at_ = ImGui::GetTime();
        }
        // The cook runs once the typing pauses.
        if (!doc_.checked() && ImGui::GetTime() - edited_at_ > 0.6) doc_.check(session.project());
    }

private:
    static int resize_cb(ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            Array<char>* buf = static_cast<Array<char>*>(data->UserData);
            buf->resize(static_cast<usize>(data->BufSize));
            data->Buf = buf->data();
        }
        return 0;
    }

    void load_buffer() {
        const StringView t = doc_.text();
        buf_.resize(t.size() + 4096);
        std::snprintf(buf_.data(), buf_.size(), "%.*s", static_cast<int>(t.size()), t.data());
    }

    void do_save(EditorSession& session) {
        cook::String err;
        if (doc_.save(session.project(), err)) {
            error_ = cook::String();
            MCE_LOG_INFO("editor: saved {}", doc_.path());
        } else {
            error_ = err;
        }
    }

    void draw_problems() {
        if (!doc_.checked()) {
            ImGui::TextDisabled("checking...");
            return;
        }
        if (doc_.problems().empty()) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "the cook accepts this text");
            return;
        }
        for (const SourceProblem& p : doc_.problems()) {
            if (p.line) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "line %u:%u  %s", p.line, p.col, p.message.c_str());
            else ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", p.message.c_str());
        }
    }

    SourceDoc doc_;
    Array<char> buf_;
    cook::String error_;
    double edited_at_ = 0;
};

}  // namespace

void add_source_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<SourcePanel>(mem::heap())); }

void open_source_panel(EditorShell& shell, asset::Guid guid) {
    if (g_panel) g_panel->open_asset(shell, guid);
}

}  // namespace mce::editor
