#include "mce/platform/library.h"

#include <SDL3/SDL.h>

#include "mce/log.h"

namespace mce::platform {

Result<void*> load_library(StringView path) {
    const FixedString<512> p(path);
    if (!file_exists(p)) return Error(Errc::NotFound, "platform.load_library", path);
    SDL_SharedObject* so = SDL_LoadObject(p.c_str());
    if (!so) {
        MCE_LOG_ERROR("platform: load_library {}: {}", p.c_str(), SDL_GetError());
        return Error(Errc::IoError, "platform.load_library", path);
    }
    return static_cast<void*>(so);
}

void* library_symbol(void* library, const char* name) {
    if (!library) return nullptr;
    return reinterpret_cast<void*>(SDL_LoadFunction(static_cast<SDL_SharedObject*>(library), name));
}

void unload_library(void* library) {
    if (library) SDL_UnloadObject(static_cast<SDL_SharedObject*>(library));
}

bool file_exists(StringView path) {
    const FixedString<512> p(path);
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

ResultV copy_file(StringView from, StringView to) {
    const FixedString<512> src(from);
    const FixedString<512> dst(to);
    if (!file_exists(src)) return Error(Errc::FileNotFound, "platform.copy_file", from);
    if (!SDL_CopyFile(src.c_str(), dst.c_str())) {
        MCE_LOG_ERROR("platform: copy_file {} -> {}: {}", src.c_str(), dst.c_str(), SDL_GetError());
        return Error(Errc::IoError, "platform.copy_file", to);
    }
    return ok();
}

ResultV create_directories(StringView path) {
    if (path.empty()) return ok();
    const FixedString<512> p(path);
    if (!SDL_CreateDirectory(p.c_str())) {  // creates the parents too; an existing directory is a success
        MCE_LOG_ERROR("platform: create_directories {}: {}", p.c_str(), SDL_GetError());
        return Error(Errc::IoError, "platform.create_directories", path);
    }
    return ok();
}

}  // namespace mce::platform
