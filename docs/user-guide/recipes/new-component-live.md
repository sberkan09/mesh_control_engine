# A new component, live

You need behaviour the engine's components do not have (engine_capabilities lists what is data and
what needs code). It goes in the game module, and the running MCP server picks it up.

1. **Declare the data** in the module's `components.h`, as the template's `Spin` and `Bob` are:

   ```cpp
   struct Patrol {
       f32 speed = 2.0f;
       f32 range = 3.0f;
       f32 origin_x = 0.0f;  // set on the first tick
       f32 time = 0.0f;
       bool started = false;
   };
   MCE_COMPONENT(Patrol, speed, range, origin_x, time, started)
   ```

2. **Write the system** in the module's game file and register both in `install()`:

   ```cpp
   void patrol(Frame& f) {
       f.query<Patrol&, LocalTransform&>().each([&](Patrol& p, LocalTransform& t) {
           if (!p.started) {
               p.origin_x = t.position.x;
               p.started = true;
           }
           p.time += f.dt;
           t.position.x = p.origin_x + p.range * std::sin(p.time * p.speed);
       });
   }
   // install(): world.register_components<Patrol>(); world.add_system(Phase::FixedUpdate_Pre, "patrol", &patrol);
   ```

3. **Build and reload.** `module_build`: errors come back as `{file, line, col, message}`; fix and
   call again. On success the server maps the new build: `refl_describe_component {"name": "Patrol"}`
   shows the fields, and the open scene is kept.
4. **Use it.** `component_add {"entity": "Guard", "component": "Patrol"}`, `scene_save`,
   `run_headless` with `trace` on `Guard`'s `LocalTransform.position.x` to see it move.
5. **Test it.** `module_test` runs the project's ctest tests (the `<game>_headless_300` smoke test
   and any you add to the project).
