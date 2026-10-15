#pragma once
// Deterministic rollback and lockstep (M-NET2 step 5).
//
// For games with a few players and a simulation that is deterministic (the engine's fixed
// timestep, no wall clock, seeded randomness): every peer runs the whole game and only inputs
// travel. Each peer simulates every frame at once with its own input, delayed `input_delay` frames,
// and a prediction of the others' (their last known input). When a remote input arrives that
// differs from the prediction, the session loads the state saved before that frame and simulates
// forward again with the real inputs: a rollback. The game never waits unless a peer falls more
// than `max_prediction` frames behind. With max_prediction 0 it is lockstep: no prediction, no
// rollback, a frame runs only when every input for it is in.
//
// Desyncs are caught, not trusted away: once every input up to a frame is known, each peer hashes
// its saved state after that frame and sends the hash along with its inputs. A peer that computed
// something else for the same frame is reported (desynced(), desync_frame()).
//
//   net::RollbackSession session;
//   session.init({.players = 2, .local_player = me, .input_size = sizeof(Input)},
//                {&game, save_state, load_state, advance_frame});
//   every tick:
//     session.add_local_input(as_bytes(read_input()));
//     session.advance();                                 // false: stalled this tick
//     for each remote player p: if (session.write_packet(p, w)) send(peer_of(p), w, Unreliable);
//   on a packet:  session.receive(bytes);
//
// The callbacks own the game state: save() writes everything the simulation reads (the same
// bytes for the same state, since they are hashed), load() puts it back, advance() runs one frame
// with every player's input. advance() also runs during a rollback, so it must not play sounds or
// spawn effects for real; do that from the newest frame's state after advance() returns.

#include "mce/containers/array.h"
#include "mce/net/bytes.h"

namespace mce::net {

inline constexpr u32 kRollbackMaxPlayers = 8;
inline constexpr u8 kRollbackMagic[3] = {'M', 'R', 'B'};
inline constexpr u8 kRollbackVersion = 1;

struct RollbackDesc {
    u32 players = 2;         // 1..kRollbackMaxPlayers
    u32 local_player = 0;    // this peer's player index
    u32 input_size = 0;      // bytes of one player's input for one frame (1..64)
    u32 input_delay = 2;     // frames between reading an input and simulating it
    u32 max_prediction = 8;  // frames the simulation may run past the confirmed frame; 0 = lockstep
};

struct RollbackCallbacks {
    void* user = nullptr;
    // Writes the whole simulation state.
    void (*save)(void* user, ByteWriter& out) = nullptr;
    // Restores a state save() wrote.
    void (*load)(void* user, Span<const u8> state) = nullptr;
    // Runs `frame` with every player's input: player p's is at inputs[p * input_size].
    void (*advance)(void* user, u32 frame, const u8* inputs, u32 input_size, u32 players) = nullptr;
};

struct RollbackStats {
    u32 rollbacks = 0;        // times a remote input proved a prediction wrong
    u32 resimulated = 0;      // frames simulated again by those rollbacks
    u32 max_depth = 0;        // the deepest rollback, in frames
    u32 stalls = 0;           // advance() calls that waited for remote inputs
    u32 predicted = 0;        // frames simulated with at least one predicted input
    u32 hashes_compared = 0;  // remote state hashes checked against ours
    u32 desyncs = 0;          // of those, the ones that differed
};

class RollbackSession {
public:
    // False for a bad description (and the session stays unusable).
    bool init(const RollbackDesc& desc, const RollbackCallbacks& callbacks);

    // The local player's input, for frame() + input_delay. Repeats in a stalled tick are ignored;
    // a frame without one repeats the previous input.
    void add_local_input(Span<const u8> input);
    // Simulates the next frame, after rolling back if a remote input disagreed with its prediction.
    // False: too far ahead of the confirmed frame (or lockstep and an input is missing); call again
    // next tick.
    bool advance();

    // The packet for remote player `player`: the local inputs it has not acknowledged, the ack of
    // its inputs, and the newest confirmed state hashes. False: nothing to send.
    bool write_packet(u32 player, ByteWriter& out);
    // A packet from any remote player (the sender is inside). False: malformed or not for this
    // session; nothing changed.
    bool receive(Span<const u8> packet);

    u32 frame() const { return frame_; }  // the next frame to simulate
    // Every input up to this frame is known (-1: none yet). The state after it is final.
    i64 confirmed_frame() const;
    // The hash of the state after a confirmed frame, while it is recent.
    bool confirmed_hash(u32 frame, u64& out) const;
    i64 hashed_frame() const { return hashed_; }
    bool desynced() const { return desync_frame_ >= 0; }
    i64 desync_frame() const { return desync_frame_; }  // the first frame found to differ
    const RollbackStats& stats() const { return stats_; }
    const RollbackDesc& desc() const { return desc_; }

private:
    static constexpr u32 kRing = 256;  // frames of inputs and hashes kept
    static constexpr u32 kMaxPacketInputs = 32;
    static constexpr u32 kPacketHashes = 16;

    struct PlayerInputs {
        Array<u8> bytes;   // kRing * input_size
        Array<u32> frame;  // the frame each slot holds
        Array<u8> known;
        i64 contiguous = -1;  // every frame up to this is known
        u32 acked = 0;        // remote: how many of our frames it has (contiguous + 1)
    };
    struct SavedState {
        u32 frame = 0;
        bool valid = false;
        Array<u8> bytes;
    };

    bool known(u32 player, u32 frame) const;
    const u8* input(u32 player, u32 frame) const;
    void set_input(u32 player, u32 frame, const u8* bytes);
    const u8* predicted_input(u32 player) const;
    void simulate(u32 frame);
    void save_state(u32 frame);
    void hash_confirmed();
    void compare_hash(u32 player, u32 frame, u64 hash);

    RollbackDesc desc_;
    RollbackCallbacks cb_;
    bool ready_ = false;
    u32 frame_ = 0;
    PlayerInputs inputs_[kRollbackMaxPlayers];
    Array<u8> used_;         // kRing * players * input_size: the inputs each frame ran with
    Array<u32> used_frame_;  // kRing
    Array<u8> zero_;         // input_size zero bytes
    Array<u8> scratch_;      // players * input_size
    Array<SavedState> states_;
    i64 wrong_ = -1;  // the earliest frame that ran with a wrong prediction
    i64 hashed_ = -1;
    u64 hashes_[kRing] = {};
    u32 hash_frame_[kRing] = {};
    bool hash_valid_[kRing] = {};
    u64 remote_hash_[kRollbackMaxPlayers][kRing] = {};
    u32 remote_hash_frame_[kRollbackMaxPlayers][kRing] = {};
    bool remote_hash_valid_[kRollbackMaxPlayers][kRing] = {};
    i64 desync_frame_ = -1;
    ByteWriter save_buf_;
    RollbackStats stats_;
};

}  // namespace mce::net
