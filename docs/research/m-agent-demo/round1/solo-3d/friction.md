# Friction backlog

Collected by mce unknown from 1 project(s).

## blocking (1)

### mcp (1)

- module_build reports reloaded:true but the server keeps the old module: new components stay unknown (MarbleGarden, solo, module_build)
  - expected: After module_build, refl_describe_component GardenGame shows the new component and entity_create can use it (recipe new-component-live.md step 3)
  - actual: module_build returns ok/rebuilt/reloaded, the log says module: loaded marble_garden v1 from build/libmarble_garden_loaded_5033_0.so, the same copy name as at startup (the counter never increments), and refl_list_components still lists the template Spin/Bob and not the new Marble/Gem/GardenGame; entity_create fails with unknown component type GardenGame. Looks like dlopen returns the already-mapped image for the reused path.
  - workaround: None from the tools; trying to keep the old component names or find another path. A server restart is not available through the bridge.
  - details: stderr also prints -- GLOB mismatch! before each reload

## major (3)

### assets (1)

- No way to make simple 3D meshes (box, sphere, ramp, disc) without writing glTF by hand (MarbleGarden, solo, library_add)
  - expected: A gen_mesh tool or library primitives (unit cube, sphere, cylinder, wedge) so a greybox 3D level can be built from data like 2D tiles
  - actual: The library has only Kenney city/platformer models; there is no primitive mesh generator. Room3D generates its glbs with a standalone python script
  - workaround: Writing a python script (adapted from Room3D tools/gen_assets.py) that emits .glb files under assets/models, then cook_run adopt

### mcp (1)

- project_set with only {title} cleared the .mceproj start_scene (MarbleGarden, solo, project_set)
  - expected: project_set {values:{title}} changes the title and keeps the other members
  - actual: The written MarbleGarden.mceproj had start_scene "" afterwards (it was the main scene GUID); project_get showed start_scene_path null. A shipped game would have started on the first scene by path or none
  - workaround: project_set {start_scene: guid:...} again and checked the file

### testing (1)

- With a module the server could not reload, run_headless / playtest / test_run play without the game systems, so bots cannot test a game whose movement is code (MarbleGarden, solo, playtest)
  - expected: playtest walk_to on the marble rolls it (the game pushes the RigidBody3D from Move) as the shipped host does
  - actual: The marble never moves (idle_fraction 0.99): the in-process run uses the stale template module. render_frame, which runs mce_player with the module from disk, does run the new code, so the two disagree silently
  - workaround: Gameplay checked through render_frame/render_sheet with scripted input and through ctest tests that run the host headless with an -input script (module_test); .mctest files only check what the engine alone does (scene loads, marble settles, no errors)

## minor (3)

### docs (1)

- engine_capabilities says 3D dynamic bodies and the character controller need code, but RigidBody3D / CharacterController3D / Trigger3D components exist (MarbleGarden, solo, engine_capabilities)
  - expected: reach: data for 3D dynamic bodies (bodies3d.h, guide 8.7)
  - actual: reach: code, how: M-DATA-REACH step 6; similarly one-shot sfx says code though SfxTrigger exists
  - workaround: Trusting the headers and guide chapter 8.7 instead

### mcp (1)

- render_frame at a late tick runs the sim in real time: tick 5600 took over two minutes (MarbleGarden, solo, render_frame)
  - expected: Reaching the capture tick fast (the sim is deterministic; it could run uncapped up to the tick before drawing), so a 90 s timer-out screen can be checked quickly
  - actual: The call outlived the 120 s client timeout; it finished in the background
  - workaround: Waited for it; for gameplay checks I run the host -headless with -input instead

### physics (1)

- Teleporting a RigidBody3D by writing LocalTransform from a FixedUpdate_Pre game system is silently lost (MarbleGarden, solo)
  - expected: bodies3d.h says moving a body entity from outside (a teleport in game code) moves the body there
  - actual: A module system in FixedUpdate_Pre runs after kit3d_bodies (the player installs the kit first and add_system only takes after), so the physics step and kit3d_writeback overwrite the new position; the marble stayed in the pond and the respawn fired every tick
  - workaround: Run the game rules in FixedUpdate_Post (after writeback) so kit3d_bodies sees the change next tick. A note in bodies3d.h / guide 8.7 on which phase to teleport from, or a before= option on add_system, would help
