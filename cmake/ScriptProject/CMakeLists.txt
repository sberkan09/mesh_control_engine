# One script project (M-SCRIPT step 5): the engine's top-level CMakeLists.txt adds this directory with
# MCE_SCRIPT_PROJECT_DIR set, so its binary directory is projects/<name> like any project's.
mce_add_script_project("${MCE_SCRIPT_PROJECT_DIR}")

# A script project's .mctest scenarios run on the stock MCP server (no module to load): the same gate
# as the M-AGENT demo games (scripts/agent_demo/demo_gate.py). Engine tree only.
if(MCE_BUILD_TESTS AND NOT MCE_SDK AND TARGET mce_mcp AND IS_DIRECTORY "${MCE_SCRIPT_PROJECT_DIR}/tests")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_filename_component(_script_project_name "${MCE_SCRIPT_PROJECT_DIR}" NAME)
    add_test(NAME ${_script_project_name}_mctests
             COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/scripts/agent_demo/demo_gate.py" --mcp $<TARGET_FILE:mce_mcp>
                     --project "${MCE_SCRIPT_PROJECT_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/gate_tests" --tests)
    set_tests_properties(${_script_project_name}_mctests PROPERTIES PASS_REGULAR_EXPRESSION "gate: pass")
endif()

# M-PERF counter gate for a script project with a perf/counters.json: the render counters may not grow. It needs a GL
# context, so it carries the gpu label like the module projects' (the gpu stage runs it under Xvfb + llvmpipe).
if(MCE_BUILD_TESTS AND NOT MCE_SDK AND TARGET mce_mcp AND EXISTS "${MCE_SCRIPT_PROJECT_DIR}/perf/counters.json")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_filename_component(_perf_project_name "${MCE_SCRIPT_PROJECT_DIR}" NAME)
    string(REGEX REPLACE "([a-z0-9])([A-Z])" "\\1_\\2" _perf_game "${_perf_project_name}")
    string(REGEX REPLACE "[^A-Za-z0-9]+" "_" _perf_game "${_perf_game}")
    string(TOLOWER "${_perf_game}" _perf_game)
    add_test(NAME ${_perf_project_name}_perf_counters
             COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/scripts/ci/perf_gate.py" --mcp $<TARGET_FILE:mce_mcp>
                     --player $<TARGET_FILE:${_perf_game}> --project "${MCE_SCRIPT_PROJECT_DIR}"
                     --work "${CMAKE_CURRENT_BINARY_DIR}/perf_gate")
    set_tests_properties(${_perf_project_name}_perf_counters PROPERTIES LABELS gpu PASS_REGULAR_EXPRESSION "gate: pass")
endif()
