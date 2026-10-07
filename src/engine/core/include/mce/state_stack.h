#pragma once
// StateStack<StateT> (core.md §2.2, FR-RUN-030): generic push/pop/replace stack of states with
// transitions deferred to apply() at the frame boundary — never mid-tick. The runtime defines
// what a state is; StateT must provide on_enter(), on_exit(), on_pause(), on_resume().

#include "mce/containers/array.h"
#include "mce/memory/allocator.h"

namespace mce {

template <class StateT>
class StateStack {
public:
    StateStack() = default;
    explicit StateStack(mem::IAllocator& a) : stack_(a), pending_(a) {}
    ~StateStack() { clear(); }
    StateStack(const StateStack&) = delete;
    StateStack& operator=(const StateStack&) = delete;

    void push(mem::UniquePtr<StateT> s) { pending_.push_back(Op{OpKind::Push, std::move(s)}); }
    void pop() { pending_.push_back(Op{OpKind::Pop, {}}); }
    void replace(mem::UniquePtr<StateT> s) { pending_.push_back(Op{OpKind::Replace, std::move(s)}); }

    // Applies queued transitions in order. Returns the number applied.
    usize apply() {
        const usize n = pending_.size();
        for (usize i = 0; i < pending_.size(); ++i) {
            Op& op = pending_[i];
            switch (op.kind) {
                case OpKind::Push:
                    if (!stack_.empty()) stack_.back()->on_pause();
                    stack_.push_back(std::move(op.state));
                    stack_.back()->on_enter();
                    break;
                case OpKind::Pop:
                    pop_now();
                    if (!stack_.empty()) stack_.back()->on_resume();
                    break;
                case OpKind::Replace:
                    pop_now();
                    stack_.push_back(std::move(op.state));
                    stack_.back()->on_enter();
                    break;
            }
        }
        pending_.clear();
        return n;
    }

    StateT* top() { return stack_.empty() ? nullptr : stack_.back().get(); }
    usize depth() const { return stack_.size(); }
    bool has_pending() const { return !pending_.empty(); }

    void clear() {
        pending_.clear();
        while (!stack_.empty()) pop_now();
    }

private:
    enum class OpKind : u8 { Push, Pop, Replace };
    struct Op {
        OpKind kind;
        mem::UniquePtr<StateT> state;
    };

    void pop_now() {
        if (stack_.empty()) return;
        stack_.back()->on_exit();
        stack_.pop_back();
    }

    SmallVector<mem::UniquePtr<StateT>, 4> stack_;
    SmallVector<Op, 4> pending_;
};

}  // namespace mce
