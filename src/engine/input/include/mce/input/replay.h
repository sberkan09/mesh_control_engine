#pragma once
// Input record/replay (FR-RUN-070, audio-input-ui §3.2 step 5). The artifact of record is the
// post-quantization InputState stream: replaying it reproduces the sim exactly, whatever the
// render rate. Encoding stores only the live part of each snapshot (~40–80 B/tick for typical
// maps).

#include "mce/containers/array.h"
#include "mce/input/actions.h"

namespace mce::input {

class InputRecorder {
public:
    explicit InputRecorder(mem::IAllocator& a = mem::heap()) : bytes_(a) {}
    void record(const InputState& s);
    u64 ticks() const { return ticks_; }
    Span<const byte> bytes() const { return bytes_.span(); }
    void clear() {
        bytes_.clear();
        ticks_ = 0;
    }

private:
    Array<byte> bytes_;
    u64 ticks_ = 0;
};

class InputReplayer {
public:
    // `stream` must outlive the replayer (bytes produced by InputRecorder).
    explicit InputReplayer(Span<const byte> stream) : stream_(stream) {}
    // Decodes the next snapshot into `out`; false at end of stream or on corrupt data.
    bool next(InputState& out);
    bool done() const { return pos_ >= stream_.size(); }

private:
    Span<const byte> stream_;
    usize pos_ = 0;
};

}  // namespace mce::input
