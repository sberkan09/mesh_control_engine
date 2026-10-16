# M-UI-DATA2 — What the M-UI-DATA blind test found (milestone record)

**Status:** active (step 1 done, 2026-10-16). Plan: ROADMAP §M-UI-DATA2; Linear SBA-153 … SBA-156.
**Why:** the M-UI-DATA blind test had a fresh agent build a title, HUD and pause menu over MCP from a one-line
brief. It succeeded, and it left four places where the agent had to work around the data instead of
saying what it meant. This milestone closes them.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | A Text widget's own alignment | SBA-153 | done |
| 2 | A document can set a value back to the kind's default over a theme | SBA-154 | not started |
| 3 | `project_check` reads `.mcui` text for size and contrast | SBA-155 | not started |
| 4 | `ui_preview` without cooking the whole project; guide and prompt | SBA-156 | not started |

## Step 1 — text alignment

**What.** `"text_align": "Left" | "Center" | "Right"` on a `.mcui` widget (`ui::TextAlign`, reflected as
`UiTextAlign`); `WidgetData::text_align`; `Document::set_text_align` / `text_align`; a "Text align" field in
the editor's UI Document inspector for Text widgets; MCP's UI document schema lists it, and
`ui_widget_set` takes it like any widget key. Text widgets place their lines across their own rect;
other kinds ignore it (buttons keep centering their label).

**How.** `Document::draw_text` passes the alignment to `render::layout_text`, which already aligned lines
inside a wrap width; the Text widget's wrap width is its rect plus the one pixel of slack SBA-61 added, so
Center and Right shift by half and one pixel to stay inside the rect. The widget record stores it in a
byte that was padding (the decoder already required padding to be zero), so older payloads read `Left`
and every existing document cooks to the same bytes.

**Gates.** `test_cook_ui`: the key cooks, round-trips through `write_ui_document` byte for byte, reaches
`ui::Document`, and an unknown value is a cook error naming `UiTextAlign`. `test_ui_layout`: a 400 px
Text's glyphs start at its left edge, center on its middle, or end at its right edge within 1.5 px,
with the same width.
