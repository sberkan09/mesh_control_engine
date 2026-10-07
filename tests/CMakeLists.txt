# Shared doctest main with the MCE assert handler wired to fail the current test.
add_library(mce_doctest_main STATIC test_main.cpp)
target_link_libraries(mce_doctest_main PUBLIC mce::doctest mce::mce_core)
target_include_directories(mce_doctest_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
mce_apply_compile_policy(mce_doctest_main)
add_library(mce::doctest_main ALIAS mce_doctest_main)

add_subdirectory(core)
add_subdirectory(devhost)
