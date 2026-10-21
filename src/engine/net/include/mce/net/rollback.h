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
//
// Spectators (M-NET3 step 2) watch without playing: a session with `spectator` set has no input
// of its own and runs only frames whose every input is known, so it never predicts or rolls back.
// One player feeds it: write_spectator_packet() carries every player's confirmed inputs, and the
// spectator's write_packet() acknowledges them. It checks the feeding player's hashes like a player
// would, so a spectator that computed something else is reported too.
//
//   player:     for each spectator s it feeds: if (session.write_spectator_packet(s, w)) send(...);
//   spectator:  session.init({.players = 2, .local_player = s, .input_size = ..., .spectator = true}, ...);
//               every tick: session.advance(); if (session.write_packet(feeder, w)) send(...);
//
// Clock sync: peers that tick at slightly different rates, or started apart, drift until the one
// ahead keeps hitting max_prediction and stalls, and the one behind keeps rolling back deep. With
// `time_sync` each packet carries the sender's frame and how far ahead it sees us; a peer that
// finds itself more than a frame ahead of another skips one tick now and then (stats().sync_waits),
// so the gap closes before it costs stalls.

#include "mce/containers/array.h"
#include "mce/net/bytes.h"

namespace mce::net {

inline constexpr u32 kRollbackMaxPlayers = 8;
inline constexpr u8 kRollbackMagic[3] = {'M', 'R', 'B'};
inline constexpr u32 kRollbackMaxSpectators = 8;
inline constexpr u8 kSpectatorMagic[3] = {'M', 'R', 'S'};     // player -> spectator: confirmed inputs
inline constexpr u8 kSpectatorAckMagic[3] = {'M', 'R', 'A'};  // spectator -> player: what it has
inline constexpr u8 kRollbackVersion = 2;

struct RollbackDesc {
    u32 players = 2;         // 1..kRollbackMaxPlayers
    u32 local_player = 0;    // this peer's player index
    u32 input_size = 0;      // bytes of one player's input for one frame (1..64)
    u32 input_delay = 2;     // frames between reading an input and simulating it
    u32 max_prediction = 8;  // frames the simulation may run past the confirmed frame; 0 = lockstep
    bool spectator = false;  // watch only: local_player is then the spectator's index (0..7)
    bool time_sync = true;   // skip a tick now and then when ahead of the other peers
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
    u32 sync_waits = 0;       // advance() calls skipped by clock sync
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
    // A spectator's write_packet(player) is its acknowledgement for the player feeding it.
    bool write_packet(u32 player, ByteWriter& out);
    // The packet for spectator `spectator` (0..kRollbackMaxSpectators-1): every player's confirmed
    // inputs it has not acknowledged and the newest hashes. False: nothing to send yet, or it fell
    // so far behind that the inputs it needs are gone (it has to be restarted).
    bool write_spectator_packet(u32 spectator, ByteWriter& out);
    // A packet from any remote player or spectator (the sender is inside). False: malformed or
    // not for this session; nothing changed.
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
    // How many frames ahead of `player` this peer runs, as clock sync estimates it (0 before the
    // first exchange, and for a spectator).
    f32 frame_advantage(u32 player) const;

private:
    static constexpr u32 kRing = 256;  // frames of inputs and hashes kept
    static constexpr u32 kMaxPacketInputs = 32;
    static constexpr u32 kPacketHashes = 16;
    static constexpr u32 kSpectatorPacketBytes = 1024;  // input bytes per spectator packet, at most
    static constexpr u32 kSpectatorCatchUp = 4;         // frames a spectator runs per advance(), at most
    static constexpr u32 kSyncInterval = 10;            // advance() calls between two sync waits

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
    bool advance_spectator();
    bool receive_spectator(ByteReader& r);
    bool receive_spectator_ack(ByteReader& r);
    void write_hashes(ByteWriter& out) const;
    bool read_hashes(ByteReader& r, u32 sender);

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
    u32 spectator_acked_[kRollbackMaxSpectators] = {};  // frames each spectator has
    f32 local_advantage_[kRollbackMaxPlayers] = {};     // our frame minus theirs when it arrived
    f32 remote_advantage_[kRollbackMaxPlayers] = {};    // the same, as they see us
    bool advantage_valid_[kRollbackMaxPlayers] = {};
    bool remote_valid_[kRollbackMaxPlayers] = {};
    u32 since_wait_ = 0;
    ByteWriter save_buf_;
    RollbackStats stats_;
};

}  // namespace mce::net
