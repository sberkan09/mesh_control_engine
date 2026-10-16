# M-UI-DATA2 — What the M-UI-DATA blind test found (milestone record)

**Status:** active (steps 1–3 done, 2026-10-16). Plan: ROADMAP §M-UI-DATA2; Linear SBA-153 … SBA-156.
**Why:** the M-UI-DATA blind test had a fresh agent build a title, HUD and pause menu over MCP from a one-line
brief. It succeeded, and it left four places where the agent had to work around the data instead of
saying what it meant. This milestone closes them.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | A Text widget's own alignment | SBA-153 | done |
| 2 | A document can set a value back to the kind's default over a theme | SBA-154 | done |
| 3 | `project_check` reads `.mcui` text for size and contrast | SBA-155 | done |
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

## Step 2 — a document resets a themed value

**What.** `"own_style": [...]` on a `.mcui` widget: any of `tint`, `text_color`, `accent`, `sprite`
(region, slice, quad), `text_size`, `letter_spacing`, `text_outline`, `text_shadow`. `apply_theme` leaves
those fields alone in every state, so a value equal to the kind's default stays the default instead of
reading as "unset". `WidgetData::own_style` (`ui::StyleOwn` bits), `Document::set_own_style` / `own_style`
(so an editor's extract and save keep it), a "Keep over the theme" group in the inspector, and the MCP
schema line; `ui_widget_set` takes it like any widget key.

**How.** The widget record's last padding byte holds the bits (the decoder already required padding to
be zero), so older payloads read 0 and every existing document cooks to the same bytes. A widget that says
`"own_style": ["tint"]` and nothing else starts from the kind's default tint (builder defaults), which is
now the value it keeps over the theme.

**Gates.** `test_cook_ui`: a themed button with `"own_style": ["tint", "text_size"]` keeps the default tint
and text size, in every state, under a theme that restyles both; the key
round-trips through `write_ui_document`; a bad name or a non-array is a cook error naming `own_style`.

## Step 3 — `project_check` reads `.mcui` labels with their theme

**What.** The accessibility check no longer reads the `.mcui` JSON as authored. It reads the document the way
the cook does (`cook::read_ui_document`: kind defaults, `"styles"`, `"own_style"`), loads the theme the
document names (`read_ui_theme`), applies it (`ui::apply_theme`), and judges every Text, Button and Toggle
label's Normal-state size and color against the nearest opaque quad behind it. A finding names the document
and the widget's path from the root (`bar/buy`), the way `ui_list` and `ui_widget_set` do. A document or theme
the cook cannot read is skipped (the cook reports it).

**Gates.** `test_mcp_check`: the seeded grey-on-grey and 9 px labels are still flagged (now by path); a
document whose theme makes buttons pale and 10 px flags a button that states neither.
