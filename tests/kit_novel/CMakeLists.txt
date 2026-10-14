# mce_kit_novel (M-NOVEL): the story VM, a novel run played by its bot, NPC dialogue saved mid-
# conversation and its default dialogue box, on a small story mce story compile builds from
# fixture/ (the build compiles it, so the compiler is tested too); and the compiler's checks
# (test_story_compile.py).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixture")
set(_header "${CMAKE_CURRENT_BINARY_DIR}/generated/novel_fixture.h")
add_custom_command(
    OUTPUT "${_header}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/story/story_compile.py" "${_fixture}"
            --out "${_header}" --report "${CMAKE_CURRENT_BINARY_DIR}/generated/REPORT.md" --no-write-loc
    DEPENDS "${PROJECT_SOURCE_DIR}/tools/story/story_compile.py" "${_fixture}/story/story.json" "${_fixture}/story/main.story"
            "${_fixture}/assets/loc/en.csv" "${_fixture}/assets/loc/tr.csv"
    COMMENT "mce story compile: the novel kit's test story"
    VERBATIM)
add_custom_target(test_novel_story DEPENDS "${_header}")

mce_add_test(test_novel UMBRELLA
    SOURCES
        test_novel.cpp
    DEPS
        mce::mce_kit_novel)
add_dependencies(test_novel test_novel_story)
target_include_directories(test_novel PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# The default dialogue box (mce_kit_novel_ui), headless.
mce_add_test(test_novel_ui UMBRELLA
    SOURCES
        test_dialogue_box.cpp
    DEPS
        mce::mce_kit_novel_ui)
add_dependencies(test_novel_ui test_novel_story)
target_include_directories(test_novel_ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_test(NAME story_compile COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_story_compile.py")
