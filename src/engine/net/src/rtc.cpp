// RtcHost (M-NET3 step 4): a room whose members also connect directly over WebRTC (mce/net/rtc.h).
//
// Every game message carries a small header, whichever way it goes:
//   through the relay  [u8 kind][u16 seq, Sequenced only][payload]   (the relay carries the channel)
//   direct             [u8 channel][u8 kind][u16 seq, Sequenced only][payload]
// kind: 0 Reliable, 1 Unreliable, 2 Sequenced, 3 signal (relay only, channel 0, reliable):
//   [3][0 offer | 1 answer][SDP]   [3][2 candidate][u8 mid length][mid][candidate]
//   [3][3 switch]                  [3][4 refuse]
// "switch" goes on the relay once the sender's direct channels are open: its Reliable messages go
// direct from then on, and the receiver holds direct Reliable messages until the switch arrives, so
// none overtakes one still on the relay.

#include "mce/net/rtc.h"

#include <chrono>
#include <cstring>

#include "mce/log.h"
#include "rtc_links.h"

namespace mce::net {

namespace {

enum : u8 { kReliable = 0, kUnreliable = 1, kSequenced = 2, kSignal = 3 };
enum : u8 { kOffer = 0, kAnswer = 1, kCandidate = 2, kSwitch = 3, kRefuse = 4 };

u64 now_us() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void append(Array<u8>& out, const void* data, usize size) {
    const usize at = out.size();
    out.resize(at + size);
    if (size) std::memcpy(out.data() + at, data, size);
}

u8 kind_of(Delivery d) {
    switch (d) {
        case Delivery::Reliable: return kReliable;
        case Delivery::Unreliable: return kUnreliable;
        case Delivery::Sequenced: return kSequenced;
    }
    return kUnreliable;
}

StringView text(Span<const u8> s) { return StringView(reinterpret_cast<const char*>(s.data()), s.size()); }
Span<const u8> bytes(StringView s) { return Span<const u8>(reinterpret_cast<const u8*>(s.data()), s.size()); }

}  // namespace

StringView rtc_path_name(RtcPath p) {
    switch (p) {
        case RtcPath::Relay: return "relay";
        case RtcPath::Connecting: return "connecting";
        case RtcPath::Direct: return "direct";
    }
    return "?";
}

RtcHost::RtcHost() = default;
RtcHost::~RtcHost() { links_.reset(); }

bool RtcHost::direct_supported() { return RtcLinks::available(); }

RtcLinks& RtcHost::links() {
    if (!links_) links_ = mem::make_unique<RtcLinks>(mem::heap());
    return *links_;
}

void RtcHost::start(Transport& link, PeerId relay, const RtcDesc& desc) {
    for (PeerId p = 0; p < kRelayMaxMembers; ++p) drop_link(p, false);
    for (Pair& pair : pairs_) pair = Pair{};
    desc_ = desc;
    ice_server_.assign(desc.ice_server);
    desc_.ice_server = {};
    channels_ = desc.channels ? desc.channels : 1;
    RelayClientDesc rd;
    rd.channels = channels_;
    rd.on_host_change = desc.on_host_change;
    rd.user = desc.user;
    room_.start(link, relay, rd);
    send_seq_.clear();
    send_seq_.resize(channels_, 0);
    recv_seq_.clear();
    recv_seq_.resize(usize(kRelayMaxMembers) * channels_, 0);
    recv_have_.clear();
    recv_have_.resize(usize(kRelayMaxMembers) * channels_, 0);
    stats_ = {};
    inbox_.clear();
    inbox_head_ = 0;
}

RtcPath RtcHost::path(PeerId peer) const { return peer < kRelayMaxMembers ? pairs_[peer].path : RtcPath::Relay; }

PeerId RtcHost::peer_of(u32 link) const {
    for (PeerId p = 0; p < kRelayMaxMembers; ++p)
        if (link != 0 && pairs_[p].link == link) return p;
    return kInvalidPeer;
}

void RtcHost::push(EventKind kind, PeerId peer, u8 channel, Span<const u8> data) {
    Queued q;
    q.event.kind = kind;
    q.event.peer = peer;
    q.event.channel = channel;
    append(q.bytes, data.data(), data.size());
    inbox_.push_back(std::move(q));
}

void RtcHost::signal(PeerId peer, u8 type, Span<const u8> a, Span<const u8> b) {
    scratch_.clear();
    const u8 head[2] = {kSignal, type};
    append(scratch_, head, 2);
    if (type == kCandidate) {
        const u8 len = static_cast<u8>(a.size() < 255 ? a.size() : 255);
        append(scratch_, &len, 1);
        append(scratch_, a.data(), len);
        append(scratch_, b.data(), b.size());
    } else {
        append(scratch_, a.data(), a.size());
    }
    room_.send(peer, 0, Span<const u8>(scratch_.data(), scratch_.size()), Delivery::Reliable);
}

void RtcHost::offer(PeerId peer) {
    Pair& pair = pairs_[peer];
    pair.link = links().open(true, ice_server_.view());
    if (pair.link == 0) {
        ++stats_.fallbacks;
        return;
    }
    pair.path = RtcPath::Connecting;
    pair.started_us = now_us();
}

void RtcHost::drop_link(PeerId peer, bool fallback) {
    Pair& pair = pairs_[peer];
    if (pair.link != 0 && links_) links_->close(pair.link);
    if (fallback && (pair.link != 0 || pair.path != RtcPath::Relay)) {
        ++stats_.fallbacks;
        MCE_LOG_INFO("net: member {} stays on the relay ({})", peer,
                     pair.path == RtcPath::Direct ? "the direct connection closed" : "no direct connection");
    }
    pair.link = 0;
    pair.path = RtcPath::Relay;
    pair.sent_switch = false;
}

void RtcHost::check_timeouts() {
    const u64 now = now_us();
    for (PeerId p = 0; p < kRelayMaxMembers; ++p) {
        const Pair& pair = pairs_[p];
        if (pair.path == RtcPath::Connecting && now - pair.started_us > u64(desc_.connect_timeout_ms) * 1000) drop_link(p, true);
    }
}

void RtcHost::deliver(PeerId peer, u8 channel, Span<const u8> payload, bool sequenced, u16 seq) {
    if (sequenced) {
        const usize i = usize(peer) * channels_ + channel;
        if (recv_have_[i] && static_cast<i16>(static_cast<u16>(seq - recv_seq_[i])) <= 0) {
            ++counters_.messages_dropped;  // older than one already delivered (it came the other way)
            return;
        }
        recv_have_[i] = 1;
        recv_seq_[i] = seq;
    }
    ++counters_.messages_received;
    counters_.payload_bytes_received += payload.size();
    push(EventKind::Received, peer, channel, payload);
}

void RtcHost::handle_signal(PeerId peer, Span<const u8> body) {
    if (body.empty()) return;
    Pair& pair = pairs_[peer];
    const Span<const u8> rest(body.data() + 1, body.size() - 1);
    switch (body[0]) {
        case kOffer:
            drop_link(peer, false);
            if (!desc_.direct || !RtcLinks::available()) {
                signal(peer, kRefuse, {});
                return;
            }
            pair.link = links().open(false, ice_server_.view());
            if (pair.link == 0) {
                ++stats_.fallbacks;
                signal(peer, kRefuse, {});
                return;
            }
            pair.path = RtcPath::Connecting;
            pair.started_us = now_us();
            links_->set_remote(pair.link, "offer", text(rest));
            break;
        case kAnswer:
            if (pair.link != 0) links_->set_remote(pair.link, "answer", text(rest));
            break;
        case kCandidate: {
            if (pair.link == 0 || rest.empty() || 1u + rest[0] > rest.size()) return;
            const StringView mid = text(Span<const u8>(rest.data() + 1, rest[0]));
            links_->add_candidate(pair.link, text(Span<const u8>(rest.data() + 1 + rest[0], rest.size() - 1 - rest[0])), mid);
            break;
        }
        case kSwitch:
            pair.got_switch = true;
            for (Queued& q : pair.held) {
                ++counters_.messages_received;
                counters_.payload_bytes_received += q.bytes.size();
                inbox_.push_back(std::move(q));
            }
            pair.held.clear();
            break;
        case kRefuse: drop_link(peer, true); break;
        default: break;
    }
}

void RtcHost::handle_relay(const Event& ev) {
    const PeerId peer = ev.peer;
    if (peer >= kRelayMaxMembers) return;
    if (ev.kind == EventKind::Connected) {
        pairs_[peer] = Pair{};
        for (u8 c = 0; c < channels_; ++c) recv_have_[usize(peer) * channels_ + c] = 0;
        push(EventKind::Connected, peer, 0, {});
        if (desc_.direct && RtcLinks::available() && room_.member_id() < peer) offer(peer);
        return;
    }
    if (ev.kind == EventKind::Disconnected) {
        drop_link(peer, false);
        pairs_[peer] = Pair{};
        push(EventKind::Disconnected, peer, 0, {});
        return;
    }
    const Span<const u8> d = ev.data;
    if (d.empty()) return;
    if (d[0] == kSignal) {
        if (ev.channel == 0) handle_signal(peer, Span<const u8>(d.data() + 1, d.size() - 1));
        return;
    }
    if (d[0] > kSequenced) return;
    const bool sequenced = d[0] == kSequenced;
    const usize head = sequenced ? 3 : 1;
    if (d.size() < head) return;
    const u16 seq = sequenced ? static_cast<u16>(d[1] | (d[2] << 8)) : 0;
    ++stats_.relay_messages_received;
    deliver(peer, ev.channel, Span<const u8>(d.data() + head, d.size() - head), sequenced, seq);
}

void RtcHost::handle_link(RtcLinkEvent& ev) {
    const PeerId peer = peer_of(ev.link);
    if (peer == kInvalidPeer) return;
    Pair& pair = pairs_[peer];
    const Span<const u8> t(ev.text.data(), ev.text.size());
    switch (ev.kind) {
        case RtcLinkEvent::Kind::Description:
            signal(peer, ev.mid.view() == "offer" ? kOffer : kAnswer, t);
            break;
        case RtcLinkEvent::Kind::Candidate: signal(peer, kCandidate, bytes(ev.mid.view()), t); break;
        case RtcLinkEvent::Kind::Open:
            if (pair.path != RtcPath::Connecting) return;
            pair.path = RtcPath::Direct;
            pair.sent_switch = true;
            ++stats_.direct_opened;
            signal(peer, kSwitch, {});
            MCE_LOG_INFO("net: member {} is connected directly", peer);
            break;
        case RtcLinkEvent::Kind::Closed: drop_link(peer, true); break;
        case RtcLinkEvent::Kind::Message: {
            if (t.size() < 2 || t[0] >= channels_ || t[1] > kSequenced) return;
            const bool sequenced = t[1] == kSequenced;
            const usize head = sequenced ? 4 : 2;
            if (t.size() < head) return;
            const u16 seq = sequenced ? static_cast<u16>(t[2] | (t[3] << 8)) : 0;
            const Span<const u8> payload(t.data() + head, t.size() - head);
            ++stats_.direct_messages_received;
            if (t[1] == kReliable && !pair.got_switch) {
                Queued q;
                q.event.kind = EventKind::Received;
                q.event.peer = peer;
                q.event.channel = t[0];
                append(q.bytes, payload.data(), payload.size());
                pair.held.push_back(std::move(q));
                return;
            }
            deliver(peer, t[0], payload, sequenced, seq);
            break;
        }
    }
}

bool RtcHost::poll(Event& out) {
    if (inbox_head_ >= inbox_.size()) {
        inbox_.clear();
        inbox_head_ = 0;
        check_timeouts();
        if (links_)
            for (RtcLinkEvent ev; links_->next(ev);) handle_link(ev);
        // One relay event at a time: a host change runs inside room_.poll(), after the game has
        // seen every event before it.
        for (Event ev; inbox_.empty() && room_.poll(ev);) handle_relay(ev);
        if (inbox_.empty()) return false;
    }
    Queued& q = inbox_[inbox_head_++];
    current_ = std::move(q.bytes);
    out = q.event;
    out.data = Span<const u8>(current_.data(), current_.size());
    return true;
}

u16 RtcHost::next_seq(u8 channel, Delivery delivery) {
    if (delivery != Delivery::Sequenced) return 0;
    return ++send_seq_[channel];
}

void RtcHost::frame(u8 channel, Span<const u8> data, Delivery delivery, u16 seq, bool direct) {
    scratch_.clear();
    if (direct) append(scratch_, &channel, 1);
    const u8 kind = kind_of(delivery);
    append(scratch_, &kind, 1);
    if (delivery == Delivery::Sequenced) {
        const u8 s[2] = {static_cast<u8>(seq & 0xff), static_cast<u8>(seq >> 8)};
        append(scratch_, s, 2);
    }
    append(scratch_, data.data(), data.size());
}

// The frame in scratch_ over the pair's direct link; false: send it through the relay instead.
bool RtcHost::send_direct(Pair& pair, Delivery delivery) {
    if (pair.path != RtcPath::Direct || !links_) return false;
    if (!links_->send(pair.link, delivery == Delivery::Reliable, Span<const u8>(scratch_.data(), scratch_.size())))
        return false;
    ++stats_.direct_messages_sent;
    return true;
}

bool RtcHost::send(PeerId peer, u8 channel, Span<const u8> data, Delivery delivery) {
    if (!is_connected(peer) || channel >= channels_) return false;
    const u16 seq = next_seq(channel, delivery);
    Pair& pair = pairs_[peer];
    bool sent = false;
    if (pair.path == RtcPath::Direct) {
        frame(channel, data, delivery, seq, true);
        sent = send_direct(pair, delivery);
    }
    if (!sent) {
        frame(channel, data, delivery, seq, false);
        if (!room_.send(peer, channel, Span<const u8>(scratch_.data(), scratch_.size()), delivery)) return false;
        ++stats_.relay_messages_sent;
    }
    ++counters_.messages_sent;
    counters_.payload_bytes_sent += data.size();
    return true;
}

void RtcHost::broadcast(u8 channel, Span<const u8> data, Delivery delivery) {
    if (channel >= channels_) return;
    const u16 seq = next_seq(channel, delivery);
    PeerId relayed[kRelayMaxMembers];
    u32 relay_count = 0;
    u32 members = 0;
    frame(channel, data, delivery, seq, true);
    for (PeerId p = 0; p < kRelayMaxMembers; ++p) {
        if (!is_connected(p)) continue;
        ++members;
        if (!send_direct(pairs_[p], delivery)) relayed[relay_count++] = p;
    }
    if (members == 0) return;
    counters_.messages_sent += members;
    counters_.payload_bytes_sent += u64(members) * data.size();
    if (relay_count == 0) return;
    frame(channel, data, delivery, seq, false);
    const Span<const u8> f(scratch_.data(), scratch_.size());
    stats_.relay_messages_sent += relay_count;
    if (relay_count == members) {
        room_.broadcast(channel, f, delivery);  // one message to the relay
        return;
    }
    for (u32 i = 0; i < relay_count; ++i) room_.send(relayed[i], channel, f, delivery);
}

void RtcHost::flush() { room_.flush(); }

void RtcHost::disconnect(PeerId peer, u32 user_data) { room_.disconnect(peer, user_data); }

}  // namespace mce::net
