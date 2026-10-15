#pragma once
// Client-side prediction and server reconciliation (M-NET2 step 3).
//
// Without prediction the local player moves only when the server's snapshot of it comes back: a
// round trip plus the jitter buffer after the key press. With it, the client runs its own inputs
// through the same step function the server uses and draws the result at once; the server stays
// authoritative:
//
//   client, every tick:                          server, for each input packet from a player:
//     const u32 seq = predictor.push(input);       receiver.receive(bytes, [&](const Input& in) {
//     if (predictor.write_inputs(w))                   step(state_of_player, in, dt);   // the same step
//         send(server, channel, w.data(), Sequenced);  });
//                                                    then, in the snapshot, the player's
//   on a snapshot with the player's state and        receiver.last_seq() (the ack), e.g. through
//   the ack:                                         ReplicationHooks::write_extra
//     predictor.reconcile(ack, server_state);
//     predictor.smooth(dt);  set LocalTransform to predictor.displayed()
//
// Inputs carry sequence numbers. Each packet repeats the newest unacknowledged ones (`redundancy`),
// so a lost packet costs nothing while a later one gets through; InputReceiver runs each input once,
// in order. reconcile() drops what the server has processed, starts again from the server's state
// and replays the rest. When the server disagreed (an input it never got, something only it knows,
// like a collision), the predicted state changes at once but the drawn transform does not jump: the
// difference becomes a visual offset that smooth() removes at `correction_rate`, or snaps when it is
// larger than `snap_distance` (a teleport).
//
// The step function must give the same result on client and server for the same state and input:
// the same code, the same dt. Inputs travel as their raw bytes (trivially copyable, little-endian on
// every target), so keep them small and validate them in the step function: the server trusts no
// client. ReplicationClient::set_predicted() keeps snapshots from drawing the predicted entity.

#include <bit>
#include <cstring>
#include <type_traits>

#include "mce/containers/array.h"
#include "mce/ecs/transform.h"
#include "mce/net/bytes.h"

namespace mce::net {

struct PredictionDesc {
    f32 dt = 1.0f / 60.0f;        // the step each input stands for, the same on the server
    u32 redundancy = 8;           // inputs per packet: the newest unacknowledged ones (1..32)
    f32 correction_rate = 10.0f;  // per second: share of a misprediction's visual error removed
    f32 snap_distance = 2.0f;     // a larger error is not smoothed but snaps (a teleport)
};

struct PredictionStats {
    u32 pending = 0;        // inputs sent and not acknowledged yet
    u32 corrections = 0;    // reconciles where the server disagreed with the prediction
    u32 snaps = 0;          // corrections too large to smooth
    f32 last_error = 0.0f;  // position error of the latest correction
    f32 offset = 0.0f;      // the visual offset still being smoothed out
};

// Input packet wire format, for tools and tests: "MIN" magic, version, input size, newest sequence
// number, count, then `count` inputs oldest first.
inline constexpr u8 kInputMagic[3] = {'M', 'I', 'N'};
inline constexpr u8 kInputVersion = 1;
inline constexpr u32 kMaxInputsPerPacket = 32;

namespace detail {

void write_input_header(ByteWriter& out, u32 input_size, u32 newest_seq, u32 count);
// Validates a packet's header against the input size; on success `in` is at the first input.
bool read_input_header(ByteReader& in, u32 input_size, u32& newest_seq, u32& count);
// Serial-number order, so a sequence that wraps keeps working.
inline bool seq_after(u32 a, u32 b) { return static_cast<i32>(a - b) > 0; }
bool same_transform(const LocalTransform& a, const LocalTransform& b);

// The visual offset between the transform drawn before a correction and the new prediction.
struct Correction {
    math::vec3 position{};
    math::quat rotation{};
    bool active = false;
    // The drawn transform was `drawn`, the prediction is now `predicted`: keep drawing `drawn`.
    void begin(const LocalTransform& drawn, const LocalTransform& predicted, f32 snap_distance, PredictionStats& stats);
    void decay(f32 rate, f32 dt);
    LocalTransform apply(const LocalTransform& predicted) const;
    f32 size() const;
};

}  // namespace detail

// Server side: one per player. Decodes the player's input packets and runs each new input once, in
// sequence order, skipping repeats; an input every packet missed is skipped (the client's
// reconcile fixes its prediction).
template <class Input>
class InputReceiver {
    static_assert(std::is_trivially_copyable_v<Input>, "inputs travel as raw bytes");
    static_assert(sizeof(Input) <= 255, "keep inputs small");

public:
    // Calls fn(const Input&) for each new input; returns how many ran, or 0 for a malformed,
    // stale or repeated packet.
    template <class Fn>
    u32 receive(Span<const u8> packet, Fn&& fn) {
        ByteReader r(packet);
        u32 newest = 0, count = 0;
        if (!detail::read_input_header(r, sizeof(Input), newest, count)) return 0;
        u32 ran = 0;
        const u32 first = newest - (count - 1);
        for (u32 i = 0; i < count; ++i) {
            Input in;
            const Span<const u8> bytes = r.read_raw(sizeof(Input));
            if (!r.ok()) return ran;
            std::memcpy(&in, bytes.data(), sizeof(Input));
            const u32 seq = first + i;
            if (has_seq_ && !detail::seq_after(seq, last_seq_)) continue;
            fn(static_cast<const Input&>(in));
            last_seq_ = seq;
            has_seq_ = true;
            ++ran;
        }
        return ran;
    }
    // The newest input that ran: send it back with the player's state as the ack (0: none yet).
    u32 last_seq() const { return last_seq_; }
    void reset() {
        last_seq_ = 0;
        has_seq_ = false;
    }

private:
    u32 last_seq_ = 0;
    bool has_seq_ = false;
};

// Client side: the predicted state of the entity the local player controls. State is the game's
// own trivially copyable struct with a `LocalTransform transform` member (what is drawn) and
// anything else the step needs (velocity, grounded, ...).
template <class Input, class State>
class Predictor {
    static_assert(std::is_trivially_copyable_v<Input>, "inputs travel as raw bytes");
    static_assert(sizeof(Input) <= 255, "keep inputs small");
    static_assert(std::endian::native == std::endian::little, "inputs travel as little-endian bytes");
    static_assert(std::is_same_v<decltype(State::transform), LocalTransform>, "State needs a LocalTransform transform");

public:
    using StepFn = void (*)(void* user, State& state, const Input& input, f32 dt);

    void init(const PredictionDesc& desc, StepFn step, void* user = nullptr) {
        desc_ = desc;
        if (desc_.redundancy < 1) desc_.redundancy = 1;
        if (desc_.redundancy > kMaxInputsPerPacket) desc_.redundancy = kMaxInputsPerPacket;
        step_ = step;
        user_ = user;
    }

    // Starts predicting from the server's state (the local entity appeared, or after a reconnect).
    // Forgets pending inputs and any visual offset.
    void reset(const State& server_state) {
        state_ = server_state;
        pending_.clear();
        correction_ = detail::Correction{};
        stats_ = PredictionStats{};
        has_ack_ = false;
        active_ = true;
    }
    void stop() { active_ = false; }
    bool active() const { return active_; }

    // One local tick: records `input` under the next sequence number and steps the prediction.
    // Returns the sequence number (0 when not active).
    u32 push(const Input& input) {
        if (!active_ || !step_) return 0;
        if (pending_.size() >= kMaxPending) {  // the server went quiet: drop the oldest
            for (usize i = 1; i < pending_.size(); ++i) pending_[i - 1] = pending_[i];
            pending_.resize(pending_.size() - 1);
        }
        pending_.push_back(Pending{++seq_, input});
        step_(user_, state_, input, desc_.dt);
        stats_.pending = static_cast<u32>(pending_.size());
        return seq_;
    }

    // The packet to send this tick: the newest unacknowledged inputs. Also call it on a tick with
    // no new input, so the last ones still reach the server. False: nothing to send.
    bool write_inputs(ByteWriter& out) const {
        if (pending_.empty()) return false;
        const u32 count = pending_.size() < desc_.redundancy ? static_cast<u32>(pending_.size()) : desc_.redundancy;
        detail::write_input_header(out, sizeof(Input), pending_[pending_.size() - 1].seq, count);
        for (usize i = pending_.size() - count; i < pending_.size(); ++i)
            out.append(Span<const u8>(reinterpret_cast<const u8*>(&pending_[i].input), sizeof(Input)));
        return true;
    }

    // The server's state of the entity after it ran inputs up to `acked_seq`: drops those inputs,
    // starts again from `server_state` and replays the rest. A disagreement becomes a visual offset.
    // An ack older than one already reconciled is ignored.
    void reconcile(u32 acked_seq, const State& server_state) {
        if (!active_ || !step_) return;
        if (has_ack_ && detail::seq_after(last_ack_, acked_seq)) return;
        last_ack_ = acked_seq;
        has_ack_ = true;
        usize keep = 0;
        while (keep < pending_.size() && !detail::seq_after(pending_[keep].seq, acked_seq)) ++keep;
        for (usize i = keep; i < pending_.size(); ++i) pending_[i - keep] = pending_[i];
        pending_.resize(pending_.size() - keep);
        stats_.pending = static_cast<u32>(pending_.size());

        const LocalTransform drawn = displayed();
        const LocalTransform before = state_.transform;
        state_ = server_state;
        for (const Pending& p : pending_) step_(user_, state_, p.input, desc_.dt);
        if (!detail::same_transform(before, state_.transform)) {
            correction_.begin(drawn, state_.transform, desc_.snap_distance, stats_);
            stats_.last_error = math::length(state_.transform.position - before.position);
        }
        stats_.offset = correction_.size();
    }

    // Removes part of the visual offset; call once per tick, after reconcile.
    void smooth(f32 dt) {
        correction_.decay(desc_.correction_rate, dt);
        stats_.offset = correction_.size();
    }

    // The predicted state (what the game logic sees) and the transform to draw (with the offset).
    const State& state() const { return state_; }
    LocalTransform displayed() const { return correction_.apply(state_.transform); }
    u32 last_seq() const { return seq_; }
    const PredictionStats& stats() const { return stats_; }

private:
    static constexpr usize kMaxPending = 256;
    struct Pending {
        u32 seq;
        Input input;
    };
    PredictionDesc desc_;
    StepFn step_ = nullptr;
    void* user_ = nullptr;
    State state_{};
    Array<Pending> pending_;
    u32 seq_ = 0;
    u32 last_ack_ = 0;
    bool has_ack_ = false;
    bool active_ = false;
    detail::Correction correction_;
    PredictionStats stats_;
};

}  // namespace mce::net
