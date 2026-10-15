#include "mce/net/sim_network.h"

#include <algorithm>
#include <cstring>

#include "link_sim.h"

namespace mce::net {
namespace {

u64 link_key(u32 from, u32 to) { return (u64(from) << 32) | to; }

Array<u8> copy_bytes(Span<const u8> data) {
    Array<u8> out;
    out.resize(data.size());
    if (!data.empty()) std::memcpy(out.data(), data.data(), data.size());
    return out;
}

}  // namespace

// ---- SimEndpoint --------------------------------------------------------------------------------

SimEndpoint::SimEndpoint(SimNetwork& network, u32 index, u32 max_peers, u8 channels)
    : network_(&network), index_(index), channels_(channels ? channels : 1) {
    peers_.resize(max_peers ? max_peers : 1);
    for (u32 i = 0; i < peers_.size(); ++i) reset_peer(i);
}

void SimEndpoint::reset_peer(PeerId peer) {
    Peer& p = peers_[peer];
    p.state = PeerState::Free;
    p.remote_endpoint = 0;
    p.remote_peer = kInvalidPeer;
    p.send_seq.clear();
    p.send_seq.resize(channels_, 0u);
    p.recv_seq.clear();
    p.recv_seq.resize(channels_, 0u);
    p.last_due.clear();
    p.last_due.resize(channels_, u64(0));
}

PeerId SimEndpoint::free_slot() const {
    for (u32 i = 0; i < peers_.size(); ++i)
        if (peers_[i].state == PeerState::Free) return i;
    return kInvalidPeer;
}

void SimEndpoint::push_event(EventKind kind, PeerId peer, u8 channel, u32 user_data, Array<u8>&& bytes) {
    Queued q;
    q.event.kind = kind;
    q.event.peer = peer;
    q.event.channel = channel;
    q.event.user_data = user_data;
    q.bytes = std::move(bytes);
    inbox_.push_back(std::move(q));
}

PeerId SimEndpoint::connect(SimEndpoint& server, u32 user_data) {
    const PeerId slot = free_slot();
    if (slot == kInvalidPeer || &server == this) return kInvalidPeer;
    reset_peer(slot);
    Peer& p = peers_[slot];
    p.state = PeerState::Connecting;
    p.remote_endpoint = server.index_;
    SimNetwork::Message m;
    m.kind = SimNetwork::Kind::Connect;
    m.from_endpoint = index_;
    m.from_peer = slot;
    m.to_endpoint = server.index_;
    m.user_data = user_data;
    network_->post(std::move(m), nullptr);
    return slot;
}

bool SimEndpoint::poll(Event& out) {
    if (inbox_head_ >= inbox_.size()) {
        inbox_.clear();
        inbox_head_ = 0;
        return false;
    }
    Queued& q = inbox_[inbox_head_++];
    current_ = std::move(q.bytes);
    out = q.event;
    out.data = Span<const u8>(current_.data(), current_.size());
    if (out.kind == EventKind::Received) {
        ++counters_.messages_received;
        counters_.payload_bytes_received += current_.size();
    }
    return true;
}

bool SimEndpoint::send(PeerId peer, u8 channel, Span<const u8> data, Delivery delivery) {
    if (!is_connected(peer) || channel >= channels_) return false;
    Peer& p = peers_[peer];
    ++counters_.messages_sent;
    counters_.payload_bytes_sent += data.size();
    SimNetwork::Message m;
    m.kind = SimNetwork::Kind::Data;
    m.from_endpoint = index_;
    m.from_peer = peer;
    m.to_endpoint = p.remote_endpoint;
    m.to_peer = p.remote_peer;
    m.channel = channel;
    m.delivery = delivery;
    if (delivery == Delivery::Sequenced) m.seq = ++p.send_seq[channel];
    m.bytes = copy_bytes(data);
    if (!network_->post(std::move(m), delivery == Delivery::Unreliable ? nullptr : &p.last_due[channel]))
        ++counters_.messages_dropped;
    return true;
}

void SimEndpoint::broadcast(u8 channel, Span<const u8> data, Delivery delivery) {
    for (u32 i = 0; i < peers_.size(); ++i)
        if (peers_[i].state == PeerState::Connected) send(i, channel, data, delivery);
}

void SimEndpoint::disconnect(PeerId peer, u32 user_data) {
    if (peer >= peers_.size() || peers_[peer].state == PeerState::Free) return;
    Peer& p = peers_[peer];
    if (p.state == PeerState::Connected) {
        // After everything ordered this peer already sent.
        u64 after = 0;
        for (u64 d : p.last_due) after = std::max(after, d);
        SimNetwork::Message m;
        m.kind = SimNetwork::Kind::Disconnect;
        m.from_endpoint = index_;
        m.from_peer = peer;
        m.to_endpoint = p.remote_endpoint;
        m.to_peer = p.remote_peer;
        m.user_data = user_data;
        network_->post(std::move(m), &after);
        push_event(EventKind::Disconnected, peer, 0, user_data, Array<u8>());
    }
    reset_peer(peer);
}

bool SimEndpoint::is_connected(PeerId peer) const {
    return peer < peers_.size() && peers_[peer].state == PeerState::Connected;
}

PeerStats SimEndpoint::stats(PeerId peer) const {
    PeerStats s;
    if (!is_connected(peer)) return s;
    const LinkConditions& out = network_->conditions(index_, peers_[peer].remote_endpoint);
    const LinkConditions& in = network_->conditions(peers_[peer].remote_endpoint, index_);
    s.rtt_ms = out.latency_ms + in.latency_ms + (out.jitter_ms + in.jitter_ms) / 2;
    s.rtt_variance_ms = (out.jitter_ms + in.jitter_ms) / 2;
    const f32 loss = std::min(1.0f, std::max(0.0f, (out.loss + in.loss) * 0.5f));
    s.packet_loss = static_cast<u32>(loss * 65536.0f);
    return s;
}

// ---- SimNetwork ---------------------------------------------------------------------------------

SimNetwork::SimNetwork(u64 seed) : rng_(detail::seed_random(seed)) {}

SimNetwork::~SimNetwork() = default;

SimEndpoint& SimNetwork::add_endpoint(u32 max_peers, u8 channels) {
    const u32 index = static_cast<u32>(endpoints_.size());
    endpoints_.push_back(mem::make_unique<SimEndpoint>(mem::heap(), *this, index, max_peers, channels));
    return *endpoints_.back();
}

void SimNetwork::set_conditions(const SimEndpoint& from, const SimEndpoint& to, const LinkConditions& c) {
    links_.insert_or_assign(link_key(from.index(), to.index()), c);
}

const LinkConditions& SimNetwork::conditions(u32 from, u32 to) const {
    const LinkConditions* c = links_.find(link_key(from, to));
    return c ? *c : default_;
}

bool SimNetwork::post(Message&& m, u64* ordered_after) {
    // Connection control travels like a reliable message: late, never lost.
    const Delivery delivery = m.kind == Kind::Data ? m.delivery : Delivery::Reliable;
    u32 delay_ms = 0;
    if (!detail::roll_delay(rng_, conditions(m.from_endpoint, m.to_endpoint), delivery, delay_ms)) return false;
    m.due_ms = now_ms_ + delay_ms;
    if (ordered_after) {
        m.due_ms = std::max(m.due_ms, *ordered_after);
        *ordered_after = m.due_ms;
    }
    m.order = next_order_++;
    flight_.push_back(std::move(m));
    return true;
}

void SimNetwork::advance(u32 ms) {
    const u64 target = now_ms_ + ms;
    // Each pass delivers what is due by `target`, in arrival order, with the clock at the
    // message's arrival, so a reply it posts (Accept, Refuse) is timed from then and lands in a
    // later pass of this same advance when it is due by `target` too.
    for (;;) {
        std::sort(flight_.begin(), flight_.end(), [](const Message& a, const Message& b) {
            return a.due_ms != b.due_ms ? a.due_ms < b.due_ms : a.order < b.order;
        });
        usize due = 0;
        while (due < flight_.size() && flight_[due].due_ms <= target) ++due;
        if (due == 0) break;
        Array<Message> arrived;
        arrived.reserve(due);
        for (usize i = 0; i < due; ++i) arrived.push_back(std::move(flight_[i]));
        for (usize i = due; i < flight_.size(); ++i) flight_[i - due] = std::move(flight_[i]);
        flight_.resize(flight_.size() - due);
        for (Message& m : arrived) {
            now_ms_ = std::max(now_ms_, m.due_ms);
            deliver(m);
        }
    }
    now_ms_ = target;
}

void SimNetwork::deliver(Message& m) {
    SimEndpoint& to = *endpoints_[m.to_endpoint];
    switch (m.kind) {
        case Kind::Connect: {
            const PeerId slot = to.free_slot();
            Message reply;
            reply.from_endpoint = m.to_endpoint;
            reply.to_endpoint = m.from_endpoint;
            reply.to_peer = m.from_peer;
            if (slot == kInvalidPeer) {
                reply.kind = Kind::Refuse;
                post(std::move(reply), nullptr);
                return;
            }
            to.reset_peer(slot);
            SimEndpoint::Peer& p = to.peers_[slot];
            p.state = SimEndpoint::PeerState::Connected;
            p.remote_endpoint = m.from_endpoint;
            p.remote_peer = m.from_peer;
            to.push_event(EventKind::Connected, slot, 0, m.user_data, Array<u8>());
            reply.kind = Kind::Accept;
            reply.from_peer = slot;
            post(std::move(reply), nullptr);
            return;
        }
        case Kind::Accept: {
            if (m.to_peer >= to.peers_.size()) return;
            SimEndpoint::Peer& p = to.peers_[m.to_peer];
            if (p.state != SimEndpoint::PeerState::Connecting || p.remote_endpoint != m.from_endpoint) return;
            p.state = SimEndpoint::PeerState::Connected;
            p.remote_peer = m.from_peer;
            to.push_event(EventKind::Connected, m.to_peer, 0, 0, Array<u8>());
            return;
        }
        case Kind::Refuse: {
            if (m.to_peer >= to.peers_.size() || to.peers_[m.to_peer].state != SimEndpoint::PeerState::Connecting)
                return;
            to.reset_peer(m.to_peer);
            to.push_event(EventKind::Disconnected, m.to_peer, 0, 0, Array<u8>());
            return;
        }
        case Kind::Disconnect: {
            if (!to.is_connected(m.to_peer)) return;
            const SimEndpoint::Peer& p = to.peers_[m.to_peer];
            if (p.remote_endpoint != m.from_endpoint || p.remote_peer != m.from_peer) return;
            to.reset_peer(m.to_peer);
            to.push_event(EventKind::Disconnected, m.to_peer, 0, m.user_data, Array<u8>());
            return;
        }
        case Kind::Data: {
            if (!to.is_connected(m.to_peer)) return;
            SimEndpoint::Peer& p = to.peers_[m.to_peer];
            if (p.remote_endpoint != m.from_endpoint || p.remote_peer != m.from_peer) return;
            if (m.delivery == Delivery::Sequenced) {
                if (m.seq <= p.recv_seq[m.channel]) {
                    ++to.counters_.messages_dropped;
                    return;
                }
                p.recv_seq[m.channel] = m.seq;
            }
            to.push_event(EventKind::Received, m.to_peer, m.channel, 0, std::move(m.bytes));
            return;
        }
    }
}

}  // namespace mce::net
