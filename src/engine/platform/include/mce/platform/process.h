#pragma once
// Child processes (the editor's run-in-player button, editor-tools.md §2.8): spawn an executable
// with arguments, poll or wait for it, optionally collect its stdout. Nothing here needs
// platform::init().

#include "mce/containers/array.h"
#include "mce/result.h"

namespace mce::platform {

class Process {
public:
    Process() = default;
    ~Process();  // releases the handle; a running child keeps running
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // args[0] is the executable path. With `capture_output`, the child's stdout (and stderr) is
    // piped for wait(); otherwise it inherits this process's console. Errc::NotFound when the
    // executable is missing, Errc::IoError when the OS refuses it.
    ResultV start(Span<const StringView> args, bool capture_output = false);
    bool started() const { return handle_ != nullptr; }
    // True while the child runs (non-blocking poll); false once it exited or was never started.
    bool running();
    // Blocks until the child exits; returns its exit code (-1 if never started). With capture,
    // `output` receives everything it wrote.
    i32 wait(Array<char>* output = nullptr);
    void kill();
    // Exit code after running() returned false or wait() returned.
    i32 exit_code() const { return exit_code_; }

private:
    void* handle_ = nullptr;
    i32 exit_code_ = -1;
    bool captured_ = false;
};

}  // namespace mce::platform
