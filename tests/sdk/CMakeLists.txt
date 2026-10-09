# The installed SDK (cmake/MceInstall.cmake): `cmake --install` this build tree into a scratch
# prefix, then a project stamped by `mce new` builds against it with find_package(MCE) (`mce
# build`), plays headless (`mce headless`), cooks with the module loaded and ships (`mce ship`).
# Where game modules are supported only (the SDK's tools load the game module).
if(NOT MCE_GAME_MODULES_SUPPORTED)
    return()
endif()
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_FOUND)
    return()
endif()
add_test(NAME sdk_install
    COMMAND ${CMAKE_COMMAND}
        -DBUILD_DIR=${PROJECT_BINARY_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/work
        -DPYTHON=${Python3_EXECUTABLE}
        -DCXX=${CMAKE_CXX_COMPILER} -DCC=${CMAKE_C_COMPILER}
        -DLAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
        -DCONFIG=${CMAKE_BUILD_TYPE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/sdk_test.cmake)
set_tests_properties(sdk_install PROPERTIES TIMEOUT 1200 LABELS sdk)
