# Undelivered — design document

**Project:** `projects/Undelivered` · **Game id:** `undelivered` · **Kind:** light novel (visual novel), 2D ·
**Purpose:** the engine's *narrative showcase*: a 40-minute branching story on the 2D kit, authored as a
project through the editor's MCP server and the SDK workflow, with a script compiler of its own ·
**Status:** design baseline (2026-10-09); §10 is updated as the game lands.

Related: [Solstice Reach](solstice-reach.md) (the graphics showcase built in the same thread),
`docs/user-guide/07-2d.md`, `12-ui-and-console.md`, `13-sequences-fsm-saves.md`, `14-mcp-ai-workflow.md`,
`projects/Kindle` (the kit-game pattern), `projects/Undertow` (lit 2D).

---

## 1. Pitch

> *Kuzgun Island, late August 1994. The ferry line closes on Sunday, and with it the post office your
> grandmother ran for forty-one years. In the back room you find a drawer she kept locked. Inside are
> seven letters she never delivered. You have six days to decide what the island gets to know.*

Undelivered is a light novel with one mechanic: each day you take one letter from the drawer and decide
what to do with it: hand it over sealed, read it first, or keep it shut. Every letter is forty years
late; every recipient is still here. What they learn changes what they do with the last week of the
ferry, and what you learn changes whether you take it.

Seven letters, six days, five endings, about 40 minutes a run and two hours to see everything.

## 2. Story

### 2.1 Setting

Kuzgun Island is a two-hour ferry from the mainland, an Aegean rock with one cove, a lighthouse, a café,
a mosque, a shut school and 140 people. It is 1994: no phones in most houses, the ferry brings the post
three times a week, and the post office is the island's only link that does not depend on weather. The
state is cancelling the ferry subsidy at the end of the summer season. Without the ferry the island will
empty in a decade, and everyone knows it.

The post office was run from 1953 until her death this spring by **Nermin Hanım**, the player's
grandmother. The player, **Deniz** (19, first year of university in İzmir finished, the grandmother's
only grandchild), has come back for the last week to pack the post office up before the directorate
collects its keys on Sunday.

### 2.2 The spine

Nermin withheld letters that would have taken people off the island. Her husband left on a ferry in 1958
and never wrote back; from then on she treated every letter that said *come*, *leave*, *there is a place
for you elsewhere* as a threat to the island, and put it in the drawer. She was not cruel. She was afraid
of an empty harbour. The island is emptying anyway, and the week of the story is the week the harbour
finally goes quiet. The question the game asks, through six letters and one ferry, is whether a truth
delivered late is still worth delivering, and whether staying can be a choice rather than something
that happens to you.

### 2.3 Cast

| Character | Age | Role in the island | Role in the story | Wants | Fears |
|---|---|---|---|---|---|
| **Deniz** (you) | 19 | Nermin's grandchild, back for a week | the deliverer; speaks in the choices | to do right by a woman they did not really know | becoming someone who leaves people behind, like the father |
| **Mira** | 20 | runs the café with her mother; Deniz's childhood friend | the *leave* arc | a conservatory place in İzmir she has already been offered | her mother's silence if she takes it |
| **Feride** | 52 | Mira's mother; owns the café; once sang at weddings | recipient of letter 2 | Mira to stay and take the café | the island with no young voice in it |
| **Selin** | 21 | the lighthouse keeper's daughter; records bird migrations in notebooks | the *stay* arc | to be the next keeper, officially | her father selling the light's post to the automation crew |
| **Halil** | 58 | lighthouse keeper since 1970; widower; sharp, dry | recipient of letter 3 | nothing he will admit to | that his staying was a mistake he made her inherit |
| **Yusuf Reis** | 68 | the last full-time fisherman; two boats, one working | recipient of letter 1 | to repair the second boat before winter | dying with his brother unspoken to |
| **Kaptan Hasan** | 61 | master of the ferry *Kuzgun II* for 30 years | recipient of letter 4; the frame of every day | to make the last crossing without a speech | what he will do on Monday |
| **Cemil** | 55 | the *muhtar* (village head); runs the grocery | recipient of letter 5 | a reason to keep the ferry | signing the island away to get it |

### 2.4 The seven letters

The drawer holds seven envelopes, oldest first. Six are deliverable from Day 2; the seventh opens on
Day 7 only.

| # | To | From | Dated | What it says | Why Nermin kept it | If delivered |
|---|---|---|---|---|---|---|
| 1 | Yusuf | his brother Kemal, Çeşme | 1971 | an apology for the quarrel over their father's boat, and an offer: half of a mainland fishing concession, "come, there is a berth" | it would have taken Yusuf | Yusuf writes back (Kemal is alive, 74); on Day 6 Kemal's son brings a boat to the cove: a line that could replace the ferry at a small scale |
| 2 | Feride | İstanbul Municipal Conservatory | 1978 | a scholarship acceptance for voice; report in October | it would have taken Feride | Feride learns she was accepted; whether she forgives the island or Mira's leaving depends on how the player handles her |
| 3 | Halil | Directorate of Coastal Safety | 1989 | approval of his request to transfer to a mainland light, after his wife's death | it would have taken Halil, and Selin | Halil learns he could have gone; his bitterness turns outward or inward; Selin learns he once wanted to leave her island |
| 4 | Hasan | Nermin herself | 1986 | a confession: she has loved him since 1962 and never said it because "a woman who keeps the post cannot be seen waiting for a ferry" | her own letter; the one she could not deliver | Hasan reads it on his bridge; what he does on Sunday changes (he stays on the island instead of retiring to the mainland) |
| 5 | Cemil | Egeport Tourism A.Ş. | 1992 | an offer to buy the cove for a hotel, with a private ferry service as part of the deal | the hotel would have changed the island | Cemil can reopen the deal: the ferry survives as a hotel shuttle, the cove is lost; the island splits over it |
| 6 | Deniz | Tarık, Deniz's father, Hamburg | 1983 | "I am not coming back to the island, but I am not leaving you. Here is where I am." An address. | her son left; she would not let him reach the child | Deniz can read it, write back from the lighthouse radio room (Selin's father relays to the mainland post), or keep it shut |
| 7 | "whoever opens this drawer" | Nermin | April 1994 | her account of the drawer, and a request: "Do not burn them. I could not deliver them. You can." | — | read on the night of Day 6; frames the ending |

### 2.5 Day structure

Seven days, Monday to Sunday. Day 1 is the prologue; Days 2 to 6 share one shape; Day 7 is the ending.

**Day 1, Monday: Arrival.** The ferry in (Hasan, the first conversation, his refusal to make a speech
about Sunday). The café (Mira; the first hint she has somewhere to go). The post office, the smell of
it, the keys. Night: the locked drawer, the small brass key in Nermin's coat, the seven envelopes laid
on the counter. Fixed; two small choices that set tone flags (how Deniz talks about Nermin; whether
Deniz tells Mira about the drawer).

**Days 2 to 6** (Tuesday to Saturday), each:

1. **Morning** at the café or the pier (alternates): a short scene with Mira or Hasan that reflects
   yesterday's delivery. The island reacts; rumours move.
2. **The drawer:** choose one of the remaining letters (1 to 6). Then choose: **Deliver sealed**,
   **Read it first**, or **Leave it for now** (no delivery today; the afternoon is longer).
   Reading shows the letter's full text and unlocks informed dialogue in the delivery; it also sets
   `read[n]`, which some recipients notice (the seal) and resent.
3. **Delivery:** a scene at the recipient's place. Each letter has a sealed and a read variant, and
   2 to 3 choice points. Its outcome sets the recipient's `trust` and one or two world flags.
4. **Afternoon:** choose who to spend it with: **Mira** (the café roof, the shut school, the ferry
   timetable), **Selin** (the lighthouse gallery, the bird notebooks, the radio room), **Yusuf** (the
   boat, the nets), or **alone** (the post office; sorting Nermin's things; the only place letter 6
   can be read). Four scenes per character over the five days, in a fixed order, with trust-gated
   lines.
5. **Night:** the journal. Deniz's one-paragraph entry (generated from the day's flags), the
   envelope count, and an autosave.

**Day 7, Sunday: The last ferry.** The morning is the whole island at the pier. Whether the ferry
leaves with Deniz on it, and who stands beside them, is decided by the state below; the player has
one final choice, and it is only offered when the state allows it.

### 2.6 State and endings

Meters are small integers written by choices; nothing is hidden from a player who pays attention,
and the journal names the day's changes in plain words.

| Variable | Range | Moved by |
|---|---|---|
| `trust[Mira]`, `trust[Selin]`, `trust[Yusuf]`, `trust[Feride]`, `trust[Halil]`, `trust[Hasan]` | −3..+6 | delivery handling, afternoons, honesty about the drawer |
| `delivered[1..6]`, `read[1..6]` | bool | the drawer |
| `cove_sold` | bool | letter 5 delivered and Cemil pressed |
| `kemal_boat` | bool | letter 1 delivered sealed or read-and-honest, and Yusuf's trust ≥ 2 |
| `feride_blessing` | bool | letter 2 delivered and Feride's scene steered to Mira |
| `halil_stays_keeper` | bool | letter 3 handled so Halil hands the light to Selin rather than the automation crew |
| `wrote_back` | bool | letter 6 read, the radio room visited with Selin, the message sent |
| `told_mira` | bool | Day 1 |
| `resolve` | 0..6 | one per letter *handled with its recipient's trust ≥ 1* (delivered or deliberately returned to them unopened with the truth of the drawer) |

Endings, checked in this order on Sunday morning:

| Ending | Condition | What happens |
|---|---|---|
| **Return Address** (secret) | `wrote_back` and `resolve ≥ 5` | the ferry brings a reply before it leaves; Deniz reads it on the pier; the game ends on the choice to stay *or* go made with the father's letter answered, and shows both last lines |
| **Open Water** | `feride_blessing` and `trust[Mira] ≥ 4` and the player chooses the ferry | Deniz and Mira leave together; Feride sings on the pier; the island keeps a line because `kemal_boat` or `cove_sold` (the epilogue text differs) |
| **Lamp Keeper** | `halil_stays_keeper` and `trust[Selin] ≥ 4` and the player chooses to stay | Deniz stays; Selin is confirmed keeper; the post office becomes the island's radio post; Hasan stays if letter 4 was delivered |
| **The Drawer Stays Shut** | `resolve ≤ 1` | Deniz puts the undelivered letters back, locks the drawer and leaves it in the directorate's box; the ferry goes; the last line is Nermin's |
| **The Last Ferry** | otherwise | Deniz leaves alone with the letters answered as far as they were; the epilogue lists what became of each recipient from the flags |

Every ending shows an epilogue card per recipient built from the flags, and the title screen's
*Letters* page marks which endings and which letter variants have been seen.

### 2.7 Tone and prose

Second person, present tense, short paragraphs; the island people speak plainly and rarely about
feelings. Humour from Hasan and Yusuf, warmth from Feride, edge from Halil. No villain: Nermin is
argued with, not condemned. The father is never seen. Turkish names, places and a few words
(*Reis*, *muhtar*, *Hanım*, *çay*) stay in Turkish with no glossary; context carries them. Script
in English; the font is cooked with the Latin-1 and Latin Extended-A ranges so the names render.

## 3. Gameplay

### 3.1 Controls

| Action | Keyboard / mouse | Gamepad | Engine action |
|---|---|---|---|
| Advance text / confirm | Space, Enter, left click | A | `Confirm`, `Fire` |
| Choose | Up/Down or W/S, then Confirm; mouse hover + click | d-pad / stick | `Navigate` (`Move`) |
| Back (close log / menu) | Esc, right click | B | `Cancel`, `Pause` |
| Backlog | L, mouse wheel up | Y | `Jump` (remapped in the project map) |
| Skip read text (hold) | Ctrl | right trigger | `TriggerR` |
| Auto advance toggle | A key | left trigger | `TriggerL` |
| Pause menu | Esc | Start | `Pause` |

The project ships its own input map (the sample pattern), so the stock names do not leak into the
screen prompts.

### 3.2 Reading

- A page is up to four lines of 58 characters at the dialogue size; the script compiler wraps and
  paginates at build time so the runtime never measures text (and so the 63-byte `TextLabel` limit is
  a page-layout fact, not a runtime surprise).
- Text reveals at 40 characters per second (typewriter, on whole UTF-8 code points); Confirm during the
  reveal completes the page; Confirm on a complete page advances. Auto mode advances after
  `0.9 s + 0.03 s × characters`. Skip advances every 4 ticks through pages already seen in any run
  (`seen` bitset is saved globally).
- The speaker's name sits on a tag at the box's top-left; narration has no tag and an italic tint.
- The backlog shows the last 64 pages with speakers, scrolling.

### 3.3 Choices

Choice rows appear in the middle of the screen over a dimmed background; 2 to 4 options, one line each.
Options can be gated: a gated option is shown greyed with its reason in brackets ("[Selin trusts you
more]") when the design wants the player to know a door exists, or hidden when it does not. Choices
have no timer. A chosen option is remembered (`seen`) and marked with a dot on later runs.

### 3.4 The drawer screen

A close-up of the drawer with the envelopes as sprites, those delivered marked with a stamp. Hover or
navigate shows the addressee and the date; Confirm opens the three-way choice. Letter 7 is a sealed
envelope with no name and refuses to open until Day 6 night.

### 3.5 Journal and saves

The night journal is the save point: one autosave slot per run plus three manual slots from the pause
menu. Save data is the state table above plus the day and node id (`Save`-flagged fields on the
`Novel` resource's mirror component, chapter 13.3). *Continue* on the title loads the autosave;
*Letters* on the title shows the global `seen` table (endings, letter variants). Saves are per user
in the standard folder.

### 3.6 Session shape

Title (the harbour at dusk, the ferry's lights, the menu) → New / Continue / Letters / Options
(text speed, auto delay, music and sfx gains bound to convars, window) → Day 1 … Day 7 → ending →
epilogue cards → credits → title.

## 4. Visual design

### 4.1 Style

**Paper and lamplight.** Backgrounds are layered paper cut-outs: flat shapes with a paper-grain
normal map, three to five parallax layers (sky, sea, far island, near buildings, foreground). The
layers are lit by `Light2D`: an ambient light that is the time of day (morning cream, noon white,
evening amber, night indigo) and point lights for lamps, the lighthouse beam, the ferry's windows and
the café's doorway. One set of shapes per location, four times of day from lighting alone, plus a
per-mood LUT through `Post2DComponent` (warm, neutral, cold, sepia for the letters) and a vignette.

Characters are paper dolls: a flat silhouette with a hair shape, a body shape, a palette per person,
and a face of a few strokes. Five expressions each (neutral, warm, troubled, sharp, open) swap the
texture; a pose change (two per character) swaps the body. They slide in from the side, bob a little
while speaking, and dim when not the speaker. Deniz is never shown.

Letters are read on a separate full-screen card: the paper, the handwriting as a different font asset,
the stamp, the date.

### 4.2 Locations

| Location | Layers | Lights | Time of day used |
|---|---|---|---|
| Pier and ferry | sky, sea, breakwater, *Kuzgun II*, pier posts | ferry windows, pier lamp | morning, evening, night, the Sunday morning |
| Café | sky through the window, harbour, room, counter, tables | doorway, table lamp, the radio's dial | morning, afternoon, night |
| Post office | shelves, counter, the drawer close-up, the window | the counter lamp | night mostly |
| Lighthouse gallery | sky, sea, the gallery rail, the lamp room behind | the light itself (a sweeping spot: a `Light2D` whose position orbits) | evening, night |
| Radio room | the desk, the set, the window | the dial, a candle | night |
| Cove and Yusuf's boats | sky, sea, the beach, two boats, nets | none by day; a brazier at night | afternoon, evening |
| Shut school | the yard, the flagpole, the window | none | afternoon |
| Cemil's grocery | shelves, the scale, the street | one bulb | afternoon |

Transitions are a 0.4-s fade through black (a full-screen quad's alpha), with the music crossfading.

### 4.3 Text and UI

Serif text for prose (Crimson Pro, OFL, 28 px baked), a slab for names, a handwriting-like face for the
letters (also OFL). The dialogue box is a paper strip across the bottom quarter with a torn top edge
(alpha), the name tag a smaller strip; choices are strips too. All UI is scene entities with
`Parallax {0,0}` so the editor places them (user guide 12.1), driven by the module.

## 5. Audio

- **Music:** six short loops synthesised offline by the generator (WAV, 48 kHz): *Harbour* (title and
  pier), *Café* (warm, guitar-like plucks), *Drawer* (sparse piano), *Light* (the lighthouse, a slow
  pad), *Cove* (a waltz for Yusuf), *Sunday* (the ending). Streamed through the music bus with a
  1.5-s crossfade on scene change.
- **Ambience:** sea wash and gulls (pier, cove), café room tone with a radio murmur, wind at the
  lighthouse, a clock in the post office. One looping `AudioSource` per location entity.
- **SFX:** page advance (a soft paper tick), choice move and confirm, envelope open (paper tear),
  the stamp, the ferry horn (Day 1 and Day 7), the lighthouse's lamp motor, the radio's static burst.

## 6. Flow

```
Title ─New─► Day1 prologue ─► [Day n: morning ─► drawer ─► (deliver | read→deliver | leave) ─► afternoon(choice) ─► journal/autosave] ×5
  │  └Continue─► journal of saved day                                                                              │
  │  └Letters (seen table)                                                                                          ▼
  └◄──────────────── credits ◄── epilogue cards ◄── ending ◄── Day 7 pier ◄───────────────────────────────── Day 6 night (letter 7)
Pause: Resume · Save (3 slots) · Load · Backlog · Options · Title
```

## 7. Technical design

### 7.1 A project on the 2D kit

`python3 scripts/new_project.py Undelivered --game undelivered`, then the template host unchanged: the
stock player runtime draws the kit, and everything the game shows is a scene entity whose fields the
module writes. The scene (`assets/scenes/main.mcscene`) is authored through the project's MCP server by
`tools/author_scene_mcp.py`: the camera, the layer entities per location (all present, toggled by
alpha), the character slots (three), the dialogue box, four text lines, the name tag, six choice rows,
the backlog panel and its lines, the title menu, the letter card, the drawer close-up and its seven
envelope sprites, the audio sources, the lights. Each UI entity carries a small `UiRole` component so
code finds it by role rather than by name.

### 7.2 The script and its compiler

The story is written in `story/*.story`, a line-based format designed for prose, and compiled by
`tools/compile_story.py` into `games/undelivered/module/story_data.h` (constant tables; the module keeps
no mutable statics, and the World holds the cursor). The compiler also wraps and paginates text with
the cooked font's metrics (so it needs the font's advance table: it reads the TTF with a tiny parser or
`fontTools` when present and falls back to a measured average), validates the graph (every label
defined, every ending reachable, no unreachable scene), and writes `story/REPORT.md` (word counts per
day and character, branch coverage).

```
# day2_yusuf.story
@scene cove time=afternoon music=cove ambience=sea
@show yusuf left neutral
Yusuf: You brought the post. There is no post.
Deniz:
  > "There was. From 1971."            -> yusuf_1971
  > "I found something of my grandmother's." -> yusuf_soft
  > [read 1] "Your brother Kemal wrote to you." -> yusuf_direct
@label yusuf_1971
@expr yusuf sharp
Yusuf: Nineteen seventy-one. [pause] Put it on the net. I will look when my hands are dry.
@set trust.yusuf +1
@flag delivered.1
@jump day2_afternoon
```

Opcodes: `say` (speaker, expression, text pages), `narrate`, `show`/`hide`/`move`/`expr` (slot,
character, pose), `bg` (location, time, lut), `music`, `ambience`, `sfx`, `choice` (options with
optional gates `[flag]`, `[trust.x >= n]`, `[!flag]`, hidden with `?`), `set`, `flag`, `unflag`,
`jump`, `call`/`return` (shared afternoon scenes), `letter` (show the letter card for letter n),
`drawer` (the drawer screen; returns the chosen letter and action), `journal`, `ending`, `end`.

### 7.3 Module

| Component / resource | Role |
|---|---|
| `Novel` (resource mirrored to a `NovelSave` component with `Save` fields) | `node`, `page`, `reveal` (characters shown), `day`, `phase`, the state table (§2.6), `seen` bits are global (a second save key) |
| `UiRole` | `kind` enum (TextLine0..3, NameTag, Box, ChoiceRow0..5, ChoiceText0..5, Slot0..2, Backlog, BacklogLine0..15, LetterCard, LetterText0..11, Envelope1..7, TitleItem0..3, Fade, LocationLayer) and `index` |
| `LocationLayer` | `location` enum, `layer` index: toggled with `tint.a` by `bg` |
| `CastSlot` | `character`, `expression`, `pose`, target x, bob phase; the module swaps `Sprite.texture` from the cast table (textures by GUID baked into the data header by the authoring script) |
| `Autopilot` (flag on `Novel`) | headless and attract mode: a deterministic choice policy (`policy` 0..4 picks the n-th option, or the option that maximises one meter) so every ending is reachable by a headless run |

Systems, in `FixedUpdate_Pre`: `undelivered_input` (action edges), `undelivered_vm` (runs the current
opcode: advance, reveal, choices, gates), `undelivered_present` (writes labels, sprites, lights, LUT,
music requests) in `FixedUpdate_Post`.

### 7.4 Tests

`undelivered_headless_300`, plus `test_undelivered` (doctest, links the module statically like
Kindle's test): the compiler's tables are consistent (every jump target exists); each of the five
policies run to an `ending` within 30 000 ticks and the five together reach all five endings; a save
mid-day and reload continues on the same node with the same state; the `seen` table marks pages
read. The MCP `run_headless` with `inspect` on `NovelSave` is the quick check used during authoring.

## 8. Writing plan

| Part | Scenes | Words (target) |
|---|---|---|
| Day 1 prologue | 4 | 1 400 |
| Mornings (Days 2–6, two variants each by yesterday's letter class) | 10 | 1 800 |
| Letters 1–6, sealed and read variants, with choice branches | 12 | 7 200 |
| Letter texts (the seven cards) | 7 | 1 600 |
| Afternoons: Mira ×5, Selin ×5, Yusuf ×5, alone ×5 | 20 | 5 000 |
| Journals (generated from fragments) | 5 × fragments | 600 |
| Day 6 night (letter 7), Day 7 pier, five endings, epilogue cards | 9 | 3 400 |
| **Total** | | **≈ 21 000** |

## 9. Production plan (this thread)

1. Project from the template; the font assets and their ranges; the generator for backgrounds,
   characters, UI strips, music and sfx (`tools/gen_assets.py`, Pillow + NumPy).
2. The compiler and the format, with Day 1 written first to drive it.
3. Module: VM, presentation, input, saves, autopilot; MCP authoring script for the scene.
4. The script, day by day; the report keeps the word counts and branch coverage honest.
5. Tests, local CI, screenshots per location and time into `/mnt/project-files/showcase-games/undelivered/`.
6. README, CREDITS (fonts), the gap log, user-guide cross-links.

## 10. Engine and SDK gaps this game exposes

| Gap | Impact | Workaround in the game | Suggested engine work |
|---|---|---|---|
| `TextLabel.text` is 63 bytes and has no wrapping or multi-line layout, although `layout_text` supports `max_width` | prose needs one entity per line and build-time pagination | the compiler paginates to 4 × 58; four `TextLabel` entities per box | a `TextBlock` kit component (longer `FixedString`, `wrap_width`, `line_spacing`) using the existing wrap |
| No sprite atlas or region | one PNG per expression and pose | 2 poses × 5 expressions × 7 characters as separate textures | `SpriteRegion` on `Sprite` |
| No screen-space UI for projects (`mce_ui` is host-only) | menus and the box are scene entities pinned with `Parallax {0,0}` | the `UiRole` pattern | a HUD anchor component, or `mce_ui` documents authored as assets |
| No per-character text reveal, rich text or italics | typewriter is the module's; italics are a tint | `reveal` counter on code points | glyph-range styling in `TextLabel` |
| No fade/transition helper | a full-screen quad whose alpha the module drives | the `Fade` role | a `ScreenFade` kit component |
| (more rows as found) | | | |
