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

# M-IMPORT3D 4a: `mce import gltf|fbx` (tools/mce_cli/mce_import_3d.py) writes a model's lights and cameras as a prefab; with
# mce_cook the FBX route (`mce_cook --dump-fbx-scene`) is checked and the prefab is cooked for real.
if(TARGET mce_cook)
    add_test(NAME import_3d
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_import_3d.py" "${CMAKE_CURRENT_BINARY_DIR}/import_3d_test"
                     "$<TARGET_FILE:mce_cook>")
else()
    add_test(NAME import_3d
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_import_3d.py" "${CMAKE_CURRENT_BINARY_DIR}/import_3d_test")
endif()

# SBA-182: `mce play` / `mce headless` warn about a pack older than the sources.
add_test(NAME cli_stale_pack
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_stale_pack.py" "${CMAKE_CURRENT_BINARY_DIR}/cli_stale_pack")

# M-CONTENT 1a: `mce cook` / `mce ship` --max-texture-size reaches mce_cook.
add_test(NAME cli_max_texture_size
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_max_texture_size.py" "${CMAKE_CURRENT_BINARY_DIR}/cli_max_texture_size")

# M-CONTENT 1b-iv: `mce cook` / `mce ship` --textures reaches mce_cook; web + bc exits; ship.py refuses bc on phones and the web.
add_test(NAME cli_textures
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_textures.py" "${CMAKE_CURRENT_BINARY_DIR}/cli_textures")

# M-CONTENT 2: `mce perf` prints the draw calls by cause from a .mcperf capture.
add_test(NAME cli_perf
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_perf.py" "${CMAKE_CURRENT_BINARY_DIR}/cli_perf")

# M-SCRIPT3 9: `mce test` plays a project's .mctest scenarios through its MCP server, no agent needed (fake server).
add_test(NAME cli_test
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_test.py" "${CMAKE_CURRENT_BINARY_DIR}/cli_test")

# M-OPENWORLD D2 / D3: the open-licence content ledger (tools/mce_cli/mce_content.py, content/ledger.json) and the humanoid
# retarget (tools/mce_cli/mce_retarget.py); with mce_cook the outputs are cooked for real.
foreach(_mce_import_py content retarget)
    if(TARGET mce_cook)
        add_test(NAME import_${_mce_import_py}
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_${_mce_import_py}.py" "${CMAKE_CURRENT_BINARY_DIR}/import_${_mce_import_py}_test"
                         "$<TARGET_FILE:mce_cook>")
    else()
        add_test(NAME import_${_mce_import_py}
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_${_mce_import_py}.py" "${CMAKE_CURRENT_BINARY_DIR}/import_${_mce_import_py}_test")
    endif()
endforeach()

# M-OPENWORLD D2: real packs from the ledger (network or MCE_CONTENT_CACHE; skips without), cooked by mce_cook. Integration run only.
if(TARGET mce_cook)
    add_test(NAME import_content_real
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_content_real.py" "${CMAKE_CURRENT_BINARY_DIR}/import_content_real_test"
                     "$<TARGET_FILE:mce_cook>")
    mce_mark_slow(import_content_real)
endif()
