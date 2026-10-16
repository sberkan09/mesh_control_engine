#include "mce/net/rollback.h"

#include <algorithm>
#include <cstring>

#include "mce/hash.h"

namespace mce::net {

bool RollbackSession::init(const RollbackDesc& desc, const RollbackCallbacks& callbacks) {
    ready_ = false;
    const u32 locals = desc.spectator ? kRollbackMaxSpectators : desc.players;
    if (desc.players < 1 || desc.players > kRollbackMaxPlayers || desc.local_player >= locals ||
        desc.input_size < 1 || desc.input_size > 64 || desc.input_delay >= kRing / 4 ||
        desc.max_prediction >= kRing / 4 || !callbacks.save || !callbacks.load || !callbacks.advance) {
        return false;
    }
    desc_ = desc;
    cb_ = callbacks;
    frame_ = 0;
    wrong_ = -1;
    hashed_ = -1;
    desync_frame_ = -1;
    stats_ = RollbackStats{};
    since_wait_ = 0;
    for (u32 i = 0; i < kRollbackMaxSpectators; ++i) spectator_acked_[i] = 0;
    for (u32 p = 0; p < kRollbackMaxPlayers; ++p) {
        local_advantage_[p] = 0.0f;
        remote_advantage_[p] = 0.0f;
        advantage_valid_[p] = false;
        remote_valid_[p] = false;
    }
    for (auto& v : hash_valid_) v = false;
    for (auto& row : remote_hash_valid_)
        for (auto& v : row) v = false;
    zero_.clear();
    zero_.resize(desc_.input_size, u8(0));
    scratch_.clear();
    scratch_.resize(static_cast<usize>(desc_.players) * desc_.input_size, u8(0));
    used_.clear();
    used_.resize(static_cast<usize>(kRing) * desc_.players * desc_.input_size, u8(0));
    used_frame_.clear();
    used_frame_.resize(kRing, ~0u);
    for (u32 p = 0; p < kRollbackMaxPlayers; ++p) {
        PlayerInputs& pi = inputs_[p];
        pi.bytes.clear();
        pi.frame.clear();
        pi.known.clear();
        pi.contiguous = -1;
        pi.acked = 0;
        if (p >= desc_.players) continue;
        pi.bytes.resize(static_cast<usize>(kRing) * desc_.input_size, u8(0));
        pi.frame.resize(kRing, ~0u);
        pi.known.resize(kRing, u8(0));
        // The first input_delay frames have no input yet for anyone: all neutral.
        for (u32 f = 0; f < desc_.input_delay; ++f) set_input(p, f, zero_.data());
    }
    // States from the oldest frame a rollback or a hash can need, up to the newest.
    states_.clear();
    states_.resize(desc_.max_prediction + 8);
    ready_ = true;
    return true;
}

bool RollbackSession::known(u32 player, u32 frame) const {
    const PlayerInputs& pi = inputs_[player];
    const u32 slot = frame % kRing;
    return pi.known[slot] && pi.frame[slot] == frame;
}

const u8* RollbackSession::input(u32 player, u32 frame) const {
    return inputs_[player].bytes.data() + static_cast<usize>(frame % kRing) * desc_.input_size;
}

void RollbackSession::set_input(u32 player, u32 frame, const u8* bytes) {
    PlayerInputs& pi = inputs_[player];
    const u32 slot = frame % kRing;
    std::memcpy(pi.bytes.data() + static_cast<usize>(slot) * desc_.input_size, bytes, desc_.input_size);
    pi.frame[slot] = frame;
    pi.known[slot] = 1;
    while (known(player, static_cast<u32>(pi.contiguous + 1))) ++pi.contiguous;
}

// A remote player's next input is guessed to be its last known one.
const u8* RollbackSession::predicted_input(u32 player) const {
    const PlayerInputs& pi = inputs_[player];
    return pi.contiguous >= 0 ? input(player, static_cast<u32>(pi.contiguous)) : zero_.data();
}

i64 RollbackSession::confirmed_frame() const {
    i64 c = inputs_[0].contiguous;
    for (u32 p = 1; p < desc_.players; ++p) c = std::min(c, inputs_[p].contiguous);
    return c;
}

void RollbackSession::add_local_input(Span<const u8> in) {
    if (!ready_ || desc_.spectator || in.size() != desc_.input_size) return;
    const u32 target = frame_ + desc_.input_delay;
    if (!known(desc_.local_player, target)) set_input(desc_.local_player, target, in.data());
}

void RollbackSession::save_state(u32 frame) {
    SavedState& s = states_[frame % states_.size()];
    save_buf_.clear();
    cb_.save(cb_.user, save_buf_);
    s.bytes.resize(save_buf_.size());
    if (save_buf_.size()) std::memcpy(s.bytes.data(), save_buf_.data().data(), save_buf_.size());
    s.frame = frame;
    s.valid = true;
}

void RollbackSession::simulate(u32 frame) {
    bool guessed = false;
    for (u32 p = 0; p < desc_.players; ++p) {
        const bool have = known(p, frame);
        guessed = guessed || !have;
        std::memcpy(scratch_.data() + static_cast<usize>(p) * desc_.input_size,
                    have ? input(p, frame) : predicted_input(p), desc_.input_size);
    }
    if (guessed) ++stats_.predicted;
    // Remember what this frame ran with, to spot a wrong guess when the real input arrives.
    const u32 slot = frame % kRing;
    std::memcpy(used_.data() + static_cast<usize>(slot) * scratch_.size(), scratch_.data(), scratch_.size());
    used_frame_[slot] = frame;
    cb_.advance(cb_.user, frame, scratch_.data(), desc_.input_size, desc_.players);
}

f32 RollbackSession::frame_advantage(u32 player) const {
    if (player >= desc_.players || !advantage_valid_[player] || !remote_valid_[player]) return 0.0f;
    // Each side's view includes the trip the frame number took; the difference cancels it.
    return (local_advantage_[player] - remote_advantage_[player]) * 0.5f;
}

bool RollbackSession::advance() {
    if (!ready_) return false;
    if (desc_.spectator) return advance_spectator();
    // The local player always has an input for this frame.
    if (!known(desc_.local_player, frame_)) {
        const PlayerInputs& me = inputs_[desc_.local_player];
        set_input(desc_.local_player, frame_,
                  me.contiguous >= 0 ? input(desc_.local_player, static_cast<u32>(me.contiguous)) : zero_.data());
    }

    // Roll back to the earliest wrong guess and simulate forward again with what is known now.
    if (wrong_ >= 0 && wrong_ < static_cast<i64>(frame_)) {
        const u32 from = static_cast<u32>(wrong_);
        const SavedState& s = states_[from % states_.size()];
        if (s.valid && s.frame == from) {
            cb_.load(cb_.user, Span<const u8>(s.bytes.data(), s.bytes.size()));
            const u32 depth = frame_ - from;
            ++stats_.rollbacks;
            stats_.resimulated += depth;
            stats_.max_depth = std::max(stats_.max_depth, depth);
            for (u32 f = from; f < frame_; ++f) {
                if (f != from) save_state(f);
                simulate(f);
            }
        }
    }
    wrong_ = -1;

    // Ahead of someone by more than a frame: let them catch up, a tick at a time.
    if (since_wait_ < kSyncInterval) ++since_wait_;
    if (desc_.time_sync && since_wait_ >= kSyncInterval) {
        f32 ahead = 0.0f;
        for (u32 p = 0; p < desc_.players; ++p)
            if (p != desc_.local_player) ahead = std::max(ahead, frame_advantage(p));
        if (ahead >= 1.0f) {
            since_wait_ = 0;
            ++stats_.sync_waits;
            hash_confirmed();
            return false;
        }
    }

    const i64 confirmed = confirmed_frame();
    if (static_cast<i64>(frame_) > confirmed + static_cast<i64>(desc_.max_prediction)) {
        ++stats_.stalls;
        hash_confirmed();
        return false;
    }
    save_state(frame_);
    simulate(frame_);
    ++frame_;
    hash_confirmed();
    return true;
}

// A spectator runs the frames whose every input is known, a few per call to catch up.
bool RollbackSession::advance_spectator() {
    u32 ran = 0;
    while (ran < kSpectatorCatchUp && static_cast<i64>(frame_) <= confirmed_frame()) {
        save_state(frame_);
        simulate(frame_);
        ++frame_;
        ++ran;
        hash_confirmed();
    }
    if (ran == 0) {
        ++stats_.stalls;
        hash_confirmed();
    }
    return ran > 0;
}

// Hashes the saved state after each newly confirmed frame (the state saved before the next one).
void RollbackSession::hash_confirmed() {
    const i64 last = std::min(confirmed_frame(), static_cast<i64>(frame_) - 2);
    while (hashed_ < last) {
        const u32 h = static_cast<u32>(hashed_ + 1);
        const SavedState& s = states_[(h + 1) % states_.size()];
        if (!s.valid || s.frame != h + 1) break;  // cannot happen with the ring sized as in init
        Hasher64 hasher;
        hasher.bytes(s.bytes.data(), s.bytes.size());
        const u32 slot = h % kRing;
        hashes_[slot] = hasher.value();
        hash_frame_[slot] = h;
        hash_valid_[slot] = true;
        hashed_ = h;
        for (u32 p = 0; p < desc_.players; ++p) {
            const bool self = !desc_.spectator && p == desc_.local_player;
            if (self || !remote_hash_valid_[p][slot] || remote_hash_frame_[p][slot] != h) continue;
            compare_hash(p, h, remote_hash_[p][slot]);
            remote_hash_valid_[p][slot] = false;
        }
    }
}

bool RollbackSession::confirmed_hash(u32 frame, u64& out) const {
    const u32 slot = frame % kRing;
    if (!hash_valid_[slot] || hash_frame_[slot] != frame) return false;
    out = hashes_[slot];
    return true;
}

void RollbackSession::compare_hash(u32 player, u32 frame, u64 hash) {
    (void)player;
    u64 mine = 0;
    if (!confirmed_hash(frame, mine)) return;
    ++stats_.hashes_compared;
    if (mine == hash) return;
    ++stats_.desyncs;
    if (desync_frame_ < 0 || static_cast<i64>(frame) < desync_frame_) desync_frame_ = frame;
}

void RollbackSession::write_hashes(ByteWriter& out) const {
    const u32 hashes = static_cast<u32>(std::min<i64>(hashed_ + 1, kPacketHashes));
    out.write_u8(static_cast<u8>(hashes));
    for (i64 f = hashed_ - static_cast<i64>(hashes) + 1; f <= hashed_; ++f) {
        u64 h = 0;
        confirmed_hash(static_cast<u32>(f), h);
        out.write_u32(static_cast<u32>(f));
        out.write_u64(h);
    }
}

// The hash section ends a packet: checked whole before anything is used.
bool RollbackSession::read_hashes(ByteReader& r, u32 sender) {
    const u32 hashes = r.read_u8();
    if (!r.ok() || hashes > kPacketHashes || r.remaining() != static_cast<usize>(hashes) * 12) return false;
    for (u32 i = 0; i < hashes; ++i) {
        const u32 f = r.read_u32();
        const u64 h = r.read_u64();
        const u32 slot = f % kRing;
        if (static_cast<i64>(f) <= hashed_) {
            compare_hash(sender, f, h);
        } else {
            remote_hash_[sender][slot] = h;
            remote_hash_frame_[sender][slot] = f;
            remote_hash_valid_[sender][slot] = true;
        }
    }
    return true;
}

bool RollbackSession::write_packet(u32 player, ByteWriter& out) {
    if (!ready_ || player >= desc_.players) return false;
    if (desc_.spectator) {
        out.append(Span<const u8>(kSpectatorAckMagic, 3));
        out.write_u8(kRollbackVersion);
        out.write_u8(static_cast<u8>(desc_.local_player));
        out.write_u32(static_cast<u32>(confirmed_frame() + 1));
        return true;
    }
    if (player == desc_.local_player) return false;
    const PlayerInputs& me = inputs_[desc_.local_player];
    const PlayerInputs& them = inputs_[player];
    // Our inputs from the first one they lack (repeated until acknowledged, so loss costs nothing).
    const u32 first = std::max(them.acked, desc_.input_delay);
    const i64 newest = me.contiguous;
    u32 count = newest >= static_cast<i64>(first) ? static_cast<u32>(newest - first + 1) : 0;
    count = std::min(count, kMaxPacketInputs);
    out.append(Span<const u8>(kRollbackMagic, 3));
    out.write_u8(kRollbackVersion);
    out.write_u8(static_cast<u8>(desc_.local_player));
    out.write_u8(static_cast<u8>(desc_.input_size));
    out.write_u32(static_cast<u32>(them.contiguous + 1));  // how many of their frames we have
    out.write_u32(frame_);                                 // for their clock sync
    out.write_u8(advantage_valid_[player] ? 1 : 0);  // nothing from them yet: no view of us
    out.write_f32(advantage_valid_[player] ? local_advantage_[player] : 0.0f);
    out.write_u32(first);
    out.write_u8(static_cast<u8>(count));
    for (u32 i = 0; i < count; ++i) out.append(Span<const u8>(input(desc_.local_player, first + i), desc_.input_size));
    write_hashes(out);  // the newest confirmed ones
    return true;
}

bool RollbackSession::write_spectator_packet(u32 spectator, ByteWriter& out) {
    if (!ready_ || desc_.spectator || spectator >= kRollbackMaxSpectators) return false;
    const u32 first = std::max(spectator_acked_[spectator], desc_.input_delay);
    const i64 last = confirmed_frame();
    u32 count = last >= static_cast<i64>(first) ? static_cast<u32>(last - first + 1) : 0;
    const u32 frame_bytes = desc_.players * desc_.input_size;
    count = std::min({count, kMaxPacketInputs, std::max(1u, kSpectatorPacketBytes / frame_bytes)});
    for (u32 i = 0; i < count; ++i)
        for (u32 p = 0; p < desc_.players; ++p)
            if (!known(p, first + i)) return false;  // overwritten: the spectator fell too far behind
    out.append(Span<const u8>(kSpectatorMagic, 3));
    out.write_u8(kRollbackVersion);
    out.write_u8(static_cast<u8>(desc_.local_player));
    out.write_u8(static_cast<u8>(desc_.players));
    out.write_u8(static_cast<u8>(desc_.input_size));
    out.write_u32(first);
    out.write_u8(static_cast<u8>(count));
    for (u32 i = 0; i < count; ++i)
        for (u32 p = 0; p < desc_.players; ++p) out.append(Span<const u8>(input(p, first + i), desc_.input_size));
    write_hashes(out);
    return true;
}

bool RollbackSession::receive(Span<const u8> packet) {
    if (!ready_) return false;
    ByteReader r(packet);
    const u8 m0 = r.read_u8(), m1 = r.read_u8(), m2 = r.read_u8();
    const u8 version = r.read_u8();
    if (!r.ok() || version != kRollbackVersion) return false;
    auto is = [&](const u8* magic) { return m0 == magic[0] && m1 == magic[1] && m2 == magic[2]; };
    if (is(kSpectatorMagic)) return desc_.spectator && receive_spectator(r);
    if (is(kSpectatorAckMagic)) return !desc_.spectator && receive_spectator_ack(r);
    if (!is(kRollbackMagic) || desc_.spectator) return false;
    const u32 sender = r.read_u8();
    const u32 size = r.read_u8();
    const u32 ack = r.read_u32();
    const u32 remote_frame = r.read_u32();
    const bool remote_valid = r.read_u8() != 0;
    const f32 remote_advantage = r.read_f32();
    const u32 first = r.read_u32();
    const u32 count = r.read_u8();
    if (!r.ok() || sender >= desc_.players || sender == desc_.local_player || size != desc_.input_size ||
        count > kMaxPacketInputs || !(remote_advantage > -1e4f && remote_advantage < 1e4f)) {
        return false;
    }
    const Span<const u8> in = r.read_raw(static_cast<usize>(count) * size);
    {
        // The hash section must be whole before anything changes.
        ByteReader check = r;
        const u32 hashes = check.read_u8();
        if (!check.ok() || hashes > kPacketHashes || check.remaining() != static_cast<usize>(hashes) * 12) return false;
    }

    // Clock sync: how far ahead of them we were when this left, smoothed over packets.
    const f32 adv = static_cast<f32>(static_cast<f64>(frame_) - static_cast<f64>(remote_frame));
    if (advantage_valid_[sender]) {
        local_advantage_[sender] += (adv - local_advantage_[sender]) * 0.125f;
    } else {
        local_advantage_[sender] = adv;
        advantage_valid_[sender] = true;
    }
    remote_advantage_[sender] = remote_advantage;
    remote_valid_[sender] = remote_valid;

    PlayerInputs& them = inputs_[sender];
    // Never trust an ack for frames we have not produced.
    const u32 produced = static_cast<u32>(inputs_[desc_.local_player].contiguous + 1);
    them.acked = std::max(them.acked, std::min(ack, produced));
    for (u32 i = 0; i < count; ++i) {
        const u32 f = first + i;
        // Inside the window we keep: not already known, not too far ahead.
        if (static_cast<i64>(f) <= them.contiguous || f >= frame_ + kRing / 2 || known(sender, f)) continue;
        const u8* bytes = in.data() + static_cast<usize>(i) * size;
        if (f < frame_ && used_frame_[f % kRing] == f &&
            std::memcmp(used_.data() + (static_cast<usize>(f % kRing) * desc_.players + sender) * size, bytes, size) !=
                0) {
            if (wrong_ < 0 || static_cast<i64>(f) < wrong_) wrong_ = f;
        }
        set_input(sender, f, bytes);
    }
    return read_hashes(r, sender);
}

bool RollbackSession::receive_spectator(ByteReader& r) {
    const u32 sender = r.read_u8();
    const u32 players = r.read_u8();
    const u32 size = r.read_u8();
    const u32 first = r.read_u32();
    const u32 count = r.read_u8();
    if (!r.ok() || sender >= desc_.players || players != desc_.players || size != desc_.input_size ||
        count > kMaxPacketInputs) {
        return false;
    }
    const usize frame_bytes = static_cast<usize>(players) * size;
    const Span<const u8> in = r.read_raw(static_cast<usize>(count) * frame_bytes);
    {
        ByteReader check = r;
        const u32 hashes = check.read_u8();
        if (!check.ok() || hashes > kPacketHashes || check.remaining() != static_cast<usize>(hashes) * 12) return false;
    }
    for (u32 i = 0; i < count; ++i) {
        const u32 f = first + i;
        if (f < frame_ || f >= frame_ + kRing / 2) continue;  // already run, or too far ahead
        for (u32 p = 0; p < players; ++p)
            if (!known(p, f)) set_input(p, f, in.data() + i * frame_bytes + static_cast<usize>(p) * size);
    }
    return read_hashes(r, sender);
}

bool RollbackSession::receive_spectator_ack(ByteReader& r) {
    const u32 spectator = r.read_u8();
    const u32 ack = r.read_u32();
    if (!r.ok() || r.remaining() != 0 || spectator >= kRollbackMaxSpectators) return false;
    // Never more than what we could have sent.
    const u32 confirmed = static_cast<u32>(confirmed_frame() + 1);
    spectator_acked_[spectator] = std::max(spectator_acked_[spectator], std::min(ack, confirmed));
    return true;
}

}  // namespace mce::net
