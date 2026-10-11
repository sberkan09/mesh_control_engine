# FetchContent PATCH_COMMAND for SDL3 (cmake/Dependencies.cmake), run in the SDL source tree:
# lets SDL GPU's Metal driver run in the iOS Simulator (M-MOBILE step 7). The Simulator's Metal device
# reports no MTLGPUFamilyApple3 although the host GPU behind it has everything SDL uses, so SDL
# refuses it; and its depth-clip call trips the Simulator's validation. Both edits sit under
# TARGET_OS_SIMULATOR, so devices and every other platform build SDL unchanged. Idempotent.

set(file "src/gpu/metal/SDL_gpu_metal.m")
file(READ "${file}" text)
if(text MATCHES "MCE: iOS Simulator")
    return()
endif()

set(family_old "            hasHardwareSupport = [device supportsFamily:MTLGPUFamilyApple3];\n        }\n")
set(family_new "${family_old}#if TARGET_OS_SIMULATOR\n        hasHardwareSupport = true; /* MCE: iOS Simulator (host GPU) */\n#endif\n")
set(clip_old "#ifndef SDL_PLATFORM_VISIONOS\n        [metalCommandBuffer->renderEncoder setDepthClipMode:")
set(clip_new "#if !defined(SDL_PLATFORM_VISIONOS) && !TARGET_OS_SIMULATOR /* MCE: iOS Simulator */\n        [metalCommandBuffer->renderEncoder setDepthClipMode:")
foreach(pair family clip)
    string(FIND "${text}" "${${pair}_old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "patch_sdl_simulator: '${pair}' anchor not found in ${file} (SDL changed: update the patch)")
    endif()
    string(REPLACE "${${pair}_old}" "${${pair}_new}" text "${text}")
endforeach()
file(WRITE "${file}" "${text}")
