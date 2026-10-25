# Editor (ARCHITECTURE §2.1, editor-tools.md §2.1, fix F7): three targets.
#   mce_editor_core  (L5) headless editor logic, no ImGui: compiles and tests in CI without a GPU
#   mce_editor_layer (L5) the only ImGui consumer: backends, dock shell, panels
#   mce_editor       (L6) the executable, a thin main()

add_subdirectory(core)
add_subdirectory(layer)
add_subdirectory(app)
add_subdirectory(mcp)
add_subdirectory(diff)
