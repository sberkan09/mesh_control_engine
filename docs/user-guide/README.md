# Mesh Control Engine — User Guide

This guide is for people **making a game** with Mesh Control Engine (MCE): where to start, how the
editor works, how to author scenes and assets, how to write gameplay code, and how to ship the
result. It is task-oriented and stays at the level of what you do and what you see.

It is **not** the engine reference. The per-subsystem reference pages (`docs/engine/`), the
architecture documents (`docs/architecture/`) and the decision records (`docs/adr/`) describe the
code as built and why; this guide links to them where you may want the full detail.

## Chapters

| # | Chapter | What you will do |
|---|---|---|
| 1 | [Getting started](01-getting-started.md) | Build the engine, run a sample, create a project, play it |
| 2 | [Projects](02-projects.md) | What a project folder holds, the `.mceproj`, the build targets |
| 3 | [The editor](03-editor.md) | Panels, viewport controls, shortcuts, play-in-editor, hot reload |
| 4 | [Scenes, entities and prefabs](04-scenes-and-prefabs.md) | Author a level, parent entities, make and override prefabs |
| 5 | [Assets and the cook](05-assets.md) | Import textures, sounds, fonts and models; cook; pack |
| 6 | [Game code](06-game-code.md) | Components, systems, phases, events, resources, the module contract |
| 7 | [2D games](07-2d.md) | Sprites, cameras, text, parallax, lighting, post effects, tilemaps, particles, pathfinding |
| 8 | [3D games](08-3d.md) | glTF models, materials, the forward renderer, the 3D physics world |
| 9 | [Physics](09-physics.md) | Bodies, colliders, the character controller, triggers, queries, joints |
| 10 | [Audio](10-audio.md) | Sound sources, one-shots, buses, 3D audio, music streams |
| 11 | [Input](11-input.md) | The action snapshot, the default map, gamepads, rebinding |
| 12 | [UI and the console](12-ui-and-console.md) | HUD text, runtime menus, the developer console, convars |
| 13 | [Sequences, state machines and saves](13-sequences-fsm-saves.md) | Scripted action lists, FSM helper, save games, replays |
| 14 | [Working with an AI agent (MCP)](14-mcp-ai-workflow.md) | The project MCP server, its tools, a proven workflow, pitfalls |
| 15 | [Shipping](15-shipping.md) | The ship zip, symbols, crash reports, version metadata, cheats |
| 16 | [Command-line reference](16-command-line.md) | Flags and convars of the player, the editor, the MCP server and the cook |
| 17 | [Troubleshooting](17-troubleshooting.md) | What a symptom means and what to do about it |
| 18 | [Samples and example projects](18-samples.md) | Where to look for a worked example of each feature |
| 19 | [Mods](19-mods.md) | Letting players add data mods: turning them on, making a mod, load order |
| 20 | [Multiplayer (optional)](20-multiplayer.md) | Opt a game into networking, run a server and clients, replicate entities |
| 21 | [Novels and dialogue (optional)](21-novels-and-dialogue.md) | A visual novel from a template, the story script, NPC dialogue in any game and its dialogue box |
| 22 | [Purchases and ads (optional)](22-purchases-and-ads.md) | Sell products and show ads: the store catalog, game code, consent, fakes for testing, store declarations |
| 23 | [Live services (optional)](23-live-services.md) | Analytics events, crash reports sent on the next launch, remote config and game services (achievements, leaderboards, cloud saves) |

## Conventions used in this guide

- Commands are written for a Linux checkout with the `linux-gcc-debug` preset. Substitute your
  preset (`msvc-debug`, `macos-debug`, `linux-clang-release`, ...) and the matching `build/<preset>/`
  directory. On Windows, executables end in `.exe` and the game module is linked into the host
  (see [Game code](06-game-code.md#platform-notes)).
- `<Project>` is a project folder under `projects/` (for example `projects/MyGame`) and `<game>`
  is its game id (`my_game`): the C++ namespace, the CMake target and the executable name.
- Units are metres, seconds and degrees unless a field says otherwise. The world is right-handed,
  Y up. 2D scenes live in the XY plane with +Y up.
- Engine state that gameplay reads each tick is called a *resource*; data attached to an entity is
  a *component*; code that runs over components every tick is a *system*.

## Where things stand

The engine is used either **from its source checkout** (projects live in `projects/<Name>/` inside
the repository and are built by the engine's own CMake) or **from the installed SDK** (headers,
prebuilt tools and `libmce_engine`, a CMake package, the `mce` command; chapter 1, section 1.9), on
Linux and macOS. The guide's commands are written for the checkout; the SDK forms are `mce build`,
`mce play`, `mce editor`, `mce mcp`, `mce cook`, `mce ship` and the stock tools with `-module`.

The current state of each feature, including what is not built yet, is in the "Known gaps"
section of the matching `docs/engine/` page. This guide repeats the gaps that change what you do.
