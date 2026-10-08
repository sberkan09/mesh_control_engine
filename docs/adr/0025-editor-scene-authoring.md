# ADR-0025: Editor scene authoring (hand-rolled 2D translate gizmo, child-process Play, autosave location)

**Status:** Accepted (2026-10-08).

**Context.** The editor catch-up implements the M2/M3/M4 editor items over the design in `editor-tools.md`:
`SceneDocument`, the `EditorScene` funnel, the preview world, the `DrawCtx` inspector, CPU picking, the translate
gizmo, run-in-player and autosave. Three decisions there were left open: the gizmo library (ImGuizmo, "via a one-page
ADR"), how Play reaches the player, and where autosaves live.

**Decisions.**

1. **The MVP translate gizmo is hand-rolled; ImGuizmo is not taken.** This is risk R8's documented fallback,
   taken up front. The MVP gizmo is 2D only: an X arrow, a Y arrow and a centre square, sized in screen pixels,
   drawn through the preview renderer's overlay lines and hit-tested in `editor_core` (`TranslateGizmo`). It is
   about 150 lines, with no ImGui in the math, so drags, axis locks, snapping and cancel are CI-tested headless.
   ImGuizmo's strengths (a 3D perspective manipulator, rotate/scale) are v0.5 work (M6 rotate/scale, M5b 3D
   viewport). They can re-open this ADR then, behind the same `TranslateGizmo` call sites.
2. **Snapping** applies to the selection *anchor*: its new position lands on the grid and the other selected
   entities keep their offsets. It is toggled in the Edit menu, and Ctrl inverts it during a drag. The step is
   independent of the visible grid step.
3. **Play is a child process.** It saves, cooks with `--pack`, and runs
   `mce_player -pack <root>/assets.mce -scene <guid>` through a new `platform::Process` (SDL3
   `SDL_CreateProcess`, so SDL stays behind the platform wall). Play never touches the editor's world, so "the
   scene reverts on Stop" holds by construction (Appendix A4). Embedded play-in-editor is M5b. The editor looks
   for the player through `-editor.player`, then next to itself (the shipped layout), then in the build tree.
4. **Autosave location:** `<project>/.mce/autosave/<scene path under assets/>.autosave`, outside `assets/`, so
   the cook and the GUID registry never see it. Untitled scenes use `untitled.mcscene`. Writes are atomic
   (temp + rename), triggered by 5 minutes of unsaved changes (`-editor.autosave_interval`) and by window focus
   loss. A save or an explicit Discard deletes the autosave. Opening a scene whose autosave differs from the file
   offers Recover / Discard / Later. Recovered work stays dirty and keeps the scene's path.
5. **Clipboard format is `.mcscene` text.** Copy writes the selected entities as a scene document. References
   between copied entities survive as `$index`; references to anything else become null. Paste assigns fresh ids
   and selects the result. Duplicate is copy + paste right after the source.
6. **Omitted `EntityRef` fields are null** in the editor's reader (the cook already got this right through explicit
   fixups); only an explicit `"$0"` names the first entity.

7. **Prefab instances and `ChildOf` (both from M6's ECS work) round-trip unchanged.** An entity's `"prefab"` link
   and its override components are kept, shown in the inspector, and copied and pasted with the entity.
   `ChildOf.parent` is an ordinary `EntityRef` field. The editor does not yet expand prefabs into the preview
   or place children through their parents in the viewport. Both belong to M6's editor half (prefab editing,
   hierarchy view).

**Consequences.** One less dependency, and the editor's interaction logic is testable without a window. Rotate and
scale handles (M6) extend `TranslateGizmo` or replace it behind the same seam. Play-in-editor (M5b) adds an embedded
host next to the child-process path; it does not replace it.
