#include "mce/platform/process.h"

#include <SDL3/SDL.h>

#include <cstring>

#include "mce/log.h"
#include "mce/platform/library.h"

namespace mce::platform {

namespace {

SDL_Process* sdl(void* h) { return static_cast<SDL_Process*>(h); }

}  // namespace

Process::~Process() {
    if (handle_) SDL_DestroyProcess(sdl(handle_));
}

ResultV Process::start(Span<const StringView> args, bool capture_output) {
    MCE_ASSERT_MSG(!handle_, "Process::start on a started process");
    if (args.empty()) return Error(Errc::InvalidArgument, "platform.process", "no executable");
    if (!file_exists(args[0])) return Error(Errc::NotFound, "platform.process", args[0]);
    // SDL wants NUL-terminated argv: pack copies into one buffer.
    Array<char> storage;
    Array<usize> offsets;
    for (StringView a : args) {
        offsets.push_back(storage.size());
        for (char c : a) storage.push_back(c);
        storage.push_back('\0');
    }
    Array<const char*> argv;
    for (usize o : offsets) argv.push_back(storage.data() + o);
    argv.push_back(nullptr);
    SDL_Process* p = SDL_CreateProcess(argv.data(), capture_output);
    if (!p) {
        MCE_LOG_ERROR("platform: cannot start {}: {}", args[0], SDL_GetError());
        return Error(Errc::IoError, "platform.process", args[0]);
    }
    handle_ = p;
    captured_ = capture_output;
    exit_code_ = -1;
    return ok();
}

bool Process::running() {
    if (!handle_) return false;
    int code = 0;
    if (SDL_WaitProcess(sdl(handle_), false, &code)) {
        exit_code_ = code;
        SDL_DestroyProcess(sdl(handle_));
        handle_ = nullptr;
        return false;
    }
    return true;
}

i32 Process::wait(Array<char>* output) {
    if (!handle_) return exit_code_;
    int code = -1;
    if (captured_) {
        size_t size = 0;
        void* data = SDL_ReadProcess(sdl(handle_), &size, &code);  // reads to EOF, then waits
        if (output && data) {
            output->resize(size);
            if (size) std::memcpy(output->data(), data, size);
        }
        SDL_free(data);
    } else {
        SDL_WaitProcess(sdl(handle_), true, &code);
    }
    exit_code_ = code;
    SDL_DestroyProcess(sdl(handle_));
    handle_ = nullptr;
    return exit_code_;
}

void Process::kill() {
    if (handle_) SDL_KillProcess(sdl(handle_), true);
}

}  // namespace mce::platform
