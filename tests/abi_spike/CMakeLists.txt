# game.dll ABI spike (ROADMAP M1, finding F33). Throwaway; superseded by the mce_engine
# umbrella DLL and the ABI ledger CI test at M3. Uses mce_core headers only, so no static
# engine library is linked into a shared image.
get_target_property(_mce_core_includes mce_core INTERFACE_INCLUDE_DIRECTORIES)

add_library(mce_spike_engine SHARED spike_engine.cpp)
add_library(mce_spike_game MODULE spike_game.cpp)
add_executable(mce_spike_host spike_host.cpp)

foreach(t mce_spike_engine mce_spike_game mce_spike_host)
    target_include_directories(${t} PRIVATE ${_mce_core_includes} ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${t} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    mce_apply_compile_policy(${t})
endforeach()

target_link_libraries(mce_spike_game PRIVATE mce_spike_engine)
target_link_libraries(mce_spike_host PRIVATE mce_spike_engine ${CMAKE_DL_LIBS})
add_dependencies(mce_spike_host mce_spike_game)

add_test(NAME abi_spike COMMAND mce_spike_host $<TARGET_FILE:mce_spike_game>)
