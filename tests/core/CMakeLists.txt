mce_add_test(test_core
    SOURCES
        test_app.cpp
        test_containers.cpp
        test_convar.cpp
        test_crash.cpp
        test_hash.cpp
        test_math.cpp
        test_memory.cpp
        test_perf_capture.cpp
        test_result.cpp
        test_state_stack.cpp
    DEPS
        mce::mce_core)
target_include_directories(test_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# DirectXMath oracle (core.md §8 test 1). Separate executable so the test-only DirectXMath
# dependency stays out of every other target (CI "DirectXMath wall").
mce_add_test(test_math_oracle
    SOURCES
        test_math_oracle.cpp
    DEPS
        mce::mce_core
        mce::directxmath)
# DirectXMath's SSE store paths type-pun (XMStoreFloat3 writes x,y through a double* via
# _mm_store_sd). That breaks strict aliasing, and GCC -O2 then reads stale floats (seen as a
# BoundingBox::CreateMerged center of 0,0 in linux-gcc-release). The oracle must not depend on
# that UB, so this one target opts out of strict aliasing.
if(NOT MSVC)
    target_compile_options(test_math_oracle PRIVATE -fno-strict-aliasing)
endif()
