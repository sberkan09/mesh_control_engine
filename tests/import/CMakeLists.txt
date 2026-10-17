# M-IMPORT: the 2D authoring-tool converters (tools/mce_cli/mce_import.py).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME import_converters
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_import.py" "${CMAKE_CURRENT_BINARY_DIR}/import_test")

# SBA-146: Spine / DragonBones skeletons (tools/mce_cli/mce_import_skel.py); with mce_cook the output is cooked for real.
if(TARGET mce_cook)
    add_test(NAME import_skeletons
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_import_skel.py" "${CMAKE_CURRENT_BINARY_DIR}/import_skel_test"
                     "$<TARGET_FILE:mce_cook>")
else()
    add_test(NAME import_skeletons
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_import_skel.py" "${CMAKE_CURRENT_BINARY_DIR}/import_skel_test")
endif()

# SBA-182: `mce play` / `mce headless` warn about a pack older than the sources.
add_test(NAME cli_stale_pack
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_stale_pack.py" "${CMAKE_CURRENT_BINARY_DIR}/cli_stale_pack")

# M-CONTENT 1a: `mce cook` / `mce ship` --max-texture-size reaches mce_cook.
add_test(NAME cli_max_texture_size
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_max_texture_size.py" "${CMAKE_CURRENT_BINARY_DIR}/cli_max_texture_size")
