// M-ACCESS step 4: the mce_ui tree a screen reader reads. Roles, names (a control's own text, else
// the label just before it), values, hidden pages left out, and the announcer's lines as focus
// moves and a focused slider changes. CPU only.

#include <doctest/doctest.h>

#include "mce/ui/access.h"

using namespace mce;
using namespace mce::ui;

namespace {

UpdateContext ctx() {
    UpdateContext c;
    c.viewport = rhi::Extent2D{1280, 720};
    return c;
}

struct Menu {
    Document doc;
    WidgetId music, subtitles, resume, filter, display_tab, audio_tab;
    Menu() {
        doc.set_modal(true);
        Builder root(doc, doc.root());
        Builder tabs = root.tabs("tabs").fill();
        Builder display = tabs.tab("display", "DISPLAY");
        Builder audio = tabs.tab("audio", "AUDIO");
        Builder list = display.stack("list");
        list.text("music_label", "MUSIC");
        music = list.slider("music", 0.0f, 1.0f, 0.1f).id();
        subtitles = list.toggle("subtitles", "Subtitles", true).id();
        list.text("filter_label", "COLOR FILTER");
        filter = list.slider("filter", 0.0f, 3.0f, 1.0f).id();
        resume = list.button("resume", "RESUME").id();
        audio.text("note", "Nothing here yet");
        display_tab = doc.find("tabs/bar/display");
        audio_tab = doc.find("tabs/bar/audio");
        doc.set_value(music, 0.4f);
        doc.set_value(filter, 2.0f);
        doc.update(ctx());
    }
};

// The first node with this name that is not a label (a label and its control share a name).
const AccessNode* named(const Array<AccessNode>& tree, StringView name) {
    const AccessNode* text = nullptr;
    for (const AccessNode& n : tree) {
        if (n.name.view() != name) continue;
        if (n.role != AccessRole::Text) return &n;
        if (!text) text = &n;
    }
    return text;
}

}  // namespace

TEST_CASE("M-ACCESS: the access tree gives each widget a role, a name and a value") {
    Menu m;
    Array<AccessNode> tree;
    build_access_tree(m.doc, tree);

    const AccessNode* music = named(tree, "Music");  // all-caps labels read in sentence case
    REQUIRE(music);
    CHECK(music->role == AccessRole::Slider);
    CHECK(music->value.view() == StringView("40%"));
    CHECK(music->widget == m.music);
    REQUIRE(music->parent >= 0);
    CHECK(tree[static_cast<usize>(music->parent)].role == AccessRole::Group);
    const AccessNode* filter = named(tree, "Color filter");
    REQUIRE(filter);
    CHECK(filter->value.view() == StringView("2"));
    const AccessNode* subs = named(tree, "Subtitles");
    REQUIRE(subs);
    CHECK(subs->role == AccessRole::Switch);
    CHECK(subs->value.view() == StringView("on"));
    const AccessNode* display = named(tree, "Display");
    REQUIRE(display);
    CHECK(display->role == AccessRole::Tab);
    CHECK(display->value.view() == StringView("selected"));
    CHECK(named(tree, "Audio")->value.empty());
    CHECK_FALSE(named(tree, "Nothing here yet"));  // the hidden page is left out
    CHECK(music->rect.size().x > 0.0f);

    FixedString<192> line;
    access_line(*music, line);
    CHECK(line.view() == StringView("Music, slider, 40%"));
    access_line(access_node(m.doc, m.resume), line);
    CHECK(line.view() == StringView("Resume, button"));
    m.doc.set_interactable(m.resume, false);
    access_line(access_node(m.doc, m.resume), line);
    CHECK(line.view() == StringView("Resume, button, dimmed"));
    access_line(access_node(m.doc, m.doc.root()), line);
    CHECK(line.empty());  // groups say nothing
}

TEST_CASE("M-ACCESS: the announcer speaks focus moves, then a focused value's changes") {
    Menu m;
    AccessAnnouncer a;
    FixedString<192> line;
    m.doc.focus(m.music);
    REQUIRE(a.next(&m.doc, line));
    CHECK(line.view() == StringView("Music, slider, 40%"));
    CHECK_FALSE(a.next(&m.doc, line));  // nothing new
    m.doc.set_value(m.music, 0.5f);
    REQUIRE(a.next(&m.doc, line));
    CHECK(line.view() == StringView("50%"));  // the value alone, as readers do for sliders
    m.doc.focus(m.subtitles);
    REQUIRE(a.next(&m.doc, line));
    CHECK(line.view() == StringView("Subtitles, switch, on"));
    m.doc.set_value(m.subtitles, 0.0f);
    REQUIRE(a.next(&m.doc, line));
    CHECK(line.view() == StringView("off"));
    CHECK_FALSE(a.next(nullptr, line));  // the menu closed
    REQUIRE(a.next(&m.doc, line));       // and came back: its focus is news again
    CHECK(line.view() == StringView("Subtitles, switch, off"));
}

TEST_CASE("M-ACCESS: a control in a labelled row says the row's label first (the remap list)") {
    Document doc;
    Builder root(doc, doc.root());
    StackLayout row_layout;
    row_layout.axis = Axis::Horizontal;
    Builder row = root.stack("row", row_layout);
    row.text("label", "Jump");
    const WidgetId key = row.button("key", "Space").id();
    const WidgetId toggle = row.toggle("toggle", "Toggle", false).id();
    Builder column = root.stack("column");  // a vertical list: its first label names only the control after it
    column.text("title", "OPTIONS");
    const WidgetId back = column.button("back", "Back").id();
    doc.update(ctx());
    FixedString<192> line;
    access_line(access_node(doc, key), line);
    CHECK(line.view() == "Jump, Space, button");
    access_line(access_node(doc, toggle), line);
    CHECK(line.view() == "Jump, Toggle, switch, off");
    access_line(access_node(doc, back), line);
    CHECK(line.view() == "Back, button");
}

TEST_CASE("access: access_role_name is what a reader says for every role") {
    const std::pair<AccessRole, const char*> all[] = {
        {AccessRole::Group, "group"},   {AccessRole::Text, "text"},     {AccessRole::Image, "image"},
        {AccessRole::Button, "button"}, {AccessRole::Switch, "switch"}, {AccessRole::Slider, "slider"},
        {AccessRole::Tab, "tab"},       {AccessRole::List, "list"},     {AccessRole::TextInput, "text field"},
    };
    for (const auto& [role, name] : all) CHECK(StringView(access_role_name(role)) == StringView(name));
}
