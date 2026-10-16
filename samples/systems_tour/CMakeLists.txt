# systems_tour: M-COVER's code-side tour. NetReplicated / NetOwner and the dialogue VM (Dialogue,
# DialogueInput) need code to start (a ReplicationServer, a compiled story), so this sample is a small
# headless program: story/ is compiled by `mce story compile` into a header at build time, and main.cpp runs
# one NPC conversation to its end. The test passes when the program prints "systems_tour: ok".
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_header "${CMAKE_CURRENT_BINARY_DIR}/generated/systems_tour_story.h")
add_custom_command(
    OUTPUT "${_header}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/story/story_compile.py" "${CMAKE_CURRENT_SOURCE_DIR}"
            --out "${_header}" --report "${CMAKE_CURRENT_BINARY_DIR}/generated/REPORT.md" --no-write-loc
    DEPENDS "${PROJECT_SOURCE_DIR}/tools/story/story_compile.py" "${CMAKE_CURRENT_SOURCE_DIR}/story/story.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/story/main.story" "${CMAKE_CURRENT_SOURCE_DIR}/assets/loc/en.csv"
    COMMENT "mce story compile: systems_tour"
    VERBATIM)
add_custom_target(systems_tour_story DEPENDS "${_header}")

mce_add_executable(sample_systems_tour UMBRELLA SOURCES main.cpp DEPS mce::mce_kit_novel)
add_dependencies(sample_systems_tour systems_tour_story)
target_include_directories(sample_systems_tour PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(MCE_BUILD_TESTS)
    add_test(NAME sample_systems_tour COMMAND sample_systems_tour -ticks 200)
    set_tests_properties(sample_systems_tour PROPERTIES PASS_REGULAR_EXPRESSION "systems_tour: ok")
endif()
