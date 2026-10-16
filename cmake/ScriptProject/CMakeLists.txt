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
