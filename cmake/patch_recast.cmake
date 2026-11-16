# FetchContent PATCH_COMMAND for recastnavigation (cmake/Dependencies.cmake), run in the recast source tree.
# DetourTileCache's simplifyContour takes `% cont.nverts` and `% cont.npoly` and reads vertex 0 without looking at
# how many vertices the contour walk produced. A tile layer with an inconsistent connection can leave a region with a
# contour of fewer than two points (a division by zero, SIGFPE, found by M-FUZZ). The nav mesh loader refuses such
# layers (physics3d navmesh3d.cpp layer_valid), and this patch is the second line: a contour that short is dropped
# (the caller already stores `nverts == 0` as an empty contour). It lives in a patch, not in a wrapper, so it survives
# a cache-cleaned FetchContent. Idempotent.

set(file "DetourTileCache/Source/DetourTileCacheBuilder.cpp")
file(READ "${file}" text)
if(text MATCHES "MCE: a contour")
    return()
endif()
set(old "static void simplifyContour(dtTempContour& cont, const float maxError)\n{\n\tcont.npoly = 0;\n")
set(new "${old}\tif (cont.nverts < 2) /* MCE: a contour of fewer than two points has nothing to simplify (and % by it is a division by zero) */\n\t{\n\t\tcont.nverts = 0;\n\t\treturn;\n\t}\n")
string(FIND "${text}" "${old}" at)
if(at EQUAL -1)
    message(FATAL_ERROR "patch_recast: simplifyContour anchor not found in ${file} (recastnavigation changed: update the patch)")
endif()
string(REPLACE "${old}" "${new}" text "${text}")
file(WRITE "${file}" "${text}")
