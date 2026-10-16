# Luau scripting (ADR-0041): the determinism harness's script case and the Luau-vs-C++ timing.
mce_add_executable(sample_script_headless UMBRELLA SOURCES main.cpp DEPS mce::mce_script_compiler)
if(MCE_BUILD_TESTS)
    add_test(NAME sample_script_headless_300 COMMAND sample_script_headless -ticks 300)
    add_test(NAME sample_script_headless_cpp_300 COMMAND sample_script_headless -ticks 300 -mode cpp)
endif()
