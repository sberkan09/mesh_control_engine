# Luau scripting spike (ADR-0039): the determinism harness's script case and the Luau-vs-C++ timing.
mce_add_executable(sample_script_headless SOURCES main.cpp DEPS mce::mce_script)
if(MCE_BUILD_TESTS)
    add_test(NAME sample_script_headless_300 COMMAND sample_script_headless -ticks 300)
    add_test(NAME sample_script_headless_cpp_300 COMMAND sample_script_headless -ticks 300 -mode cpp)
endif()
