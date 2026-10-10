#include "mce/net/host.h"

#include <enet/enet.h>

#include <atomic>
#include <mutex>

#include "mce/assert.h"
#include "mce/log.h"

namespace mce::net {
namespace {

// enet_initialize / enet_deinitialize are process-wide (WSAStartup on Windows): counted per Host.
std::mutex g_init_mutex;
u32 g_init_count = 0;

bool acquire_enet() {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_init_count == 0 && enet_initialize() != 0) {
        MCE_LOG_ERROR("net: enet_initialize failed");
        return false;
    }
    ++g_init_count;
    return true;
}

void release_enet() {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    MCE_ASSERT(g_init_count > 0);
    if (--g_init_count == 0) enet_deinitialize();
}

ENetHost* as_host(void* h) { return static_cast<ENetHost*>(h); }

ENetPeer* peer_at(void* h, PeerId id) {
    ENetHost* host = as_host(h);
    if (!host || id >= host->peerCount) return nullptr;
    return &host->peers[id];
}

PeerId peer_id(const ENetHost* host, const ENetPeer* peer) { return static_cast<PeerId>(peer - host->peers); }

enet_uint32 packet_flags(Delivery d) {
    switch (d) {
        case Delivery::Reliable: return ENET_PACKET_FLAG_RELIABLE;
        case Delivery::Sequenced: return 0;
        case Delivery::Unreliable: return ENET_PACKET_FLAG_UNSEQUENCED;
    }
    return ENET_PACKET_FLAG_RELIABLE;
}

// Port 0 / "": ENet's "any". Names resolve through enet_address_set_host.
bool resolve(StringView name, u16 port, ENetAddress& out) {
    out.host = ENET_HOST_ANY;
    out.port = port;
    if (name.empty()) return true;
    FixedString<256> z(name);
    return enet_address_set_host(&out, z.c_str()) == 0;
}

}  // namespace

Host::Host() = default;

Host::~Host() { close(); }

bool Host::create(const HostDesc& desc, bool bind) {
    close();
    if (desc.channels == 0 || desc.max_peers == 0 || desc.max_peers > ENET_PROTOCOL_MAXIMUM_PEER_ID) {
        MCE_LOG_ERROR("net: invalid host description (channels {}, max_peers {})", desc.channels, desc.max_peers);
        return false;
    }
    if (!acquire_enet()) return false;
    ENetAddress address{};
    if (bind && !resolve(desc.bind_address.view(), desc.port, address)) {
        MCE_LOG_ERROR("net: cannot resolve bind address '{}'", desc.bind_address.view());
        release_enet();
        return false;
    }
    ENetHost* host = enet_host_create(bind ? &address : nullptr, desc.max_peers, desc.channels,
                                      desc.incoming_bandwidth, desc.outgoing_bandwidth);
    if (!host) {
        MCE_LOG_ERROR("net: cannot open a UDP endpoint{} (port {})", bind ? " for listening" : "", desc.port);
        release_enet();
        return false;
    }
    host_ = host;
    channels_ = desc.channels;
    timeout_ms_ = desc.timeout_ms;
    connected_.clear();
    connected_.resize(host->peerCount, u8(0));
    ENetAddress local{};
    port_ = enet_socket_get_address(host->socket, &local) == 0 ? local.port : desc.port;
    return true;
}

bool Host::listen(const HostDesc& desc) {
    if (!create(desc, true)) return false;
    MCE_LOG_INFO("net: listening on port {} ({} peers, {} channels)", port_, desc.max_peers, desc.channels);
    return true;
}

bool Host::open_client(const HostDesc& desc) { return create(desc, false); }

PeerId Host::connect(StringView host_name, u16 port, u32 user_data) {
    ENetHost* host = as_host(host_);
    if (!host) return kInvalidPeer;
    ENetAddress address{};
    if (host_name.empty() || !resolve(host_name, port, address)) {
        MCE_LOG_ERROR("net: cannot resolve '{}'", host_name);
        return kInvalidPeer;
    }
    ENetPeer* peer = enet_host_connect(host, &address, channels_, user_data);
    if (!peer) {
        MCE_LOG_ERROR("net: no free peer slot to connect to {}:{}", host_name, port);
        return kInvalidPeer;
    }
    // Minimum timeout lower bound (ENet default 5 s) and maximum (ENet default 30 s): both follow
    // the description so a dead server is reported in timeout_ms.
    enet_peer_timeout(peer, 0, timeout_ms_, timeout_ms_);
    return peer_id(host, peer);
}

void Host::release_received() {
    if (received_) {
        enet_packet_destroy(static_cast<ENetPacket*>(received_));
        received_ = nullptr;
    }
}

bool Host::poll(Event& out, u32 timeout_ms) {
    release_received();
    ENetHost* host = as_host(host_);
    if (!host) return false;
    ENetEvent ev{};
    const int r = enet_host_service(host, &ev, timeout_ms);
    if (r < 0) {
        MCE_LOG_WARN("net: socket error while servicing the host");
        return false;
    }
    if (r == 0) return false;
    out = Event{};
    out.peer = peer_id(host, ev.peer);
    switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            out.kind = EventKind::Connected;
            out.user_data = ev.data;
            connected_[out.peer] = 1;
            enet_peer_timeout(ev.peer, 0, timeout_ms_, timeout_ms_);
            return true;
        case ENET_EVENT_TYPE_DISCONNECT:
            out.kind = EventKind::Disconnected;
            out.user_data = ev.data;
            connected_[out.peer] = 0;
            return true;
        case ENET_EVENT_TYPE_RECEIVE:
            out.kind = EventKind::Received;
            out.channel = ev.channelID;
            out.data = Span<const u8>(ev.packet->data, ev.packet->dataLength);
            received_ = ev.packet;
            return true;
        case ENET_EVENT_TYPE_NONE: break;
    }
    return false;
}

bool Host::send(PeerId peer, u8 channel, Span<const u8> data, Delivery delivery) {
    ENetPeer* p = peer_at(host_, peer);
    if (!p || !connected_[peer] || channel >= channels_) return false;
    ENetPacket* packet = enet_packet_create(data.data(), data.size(), packet_flags(delivery));
    if (!packet) return false;
    if (enet_peer_send(p, channel, packet) != 0) {
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

void Host::broadcast(u8 channel, Span<const u8> data, Delivery delivery) {
    ENetHost* host = as_host(host_);
    if (!host || channel >= channels_ || connected_count() == 0) return;
    ENetPacket* packet = enet_packet_create(data.data(), data.size(), packet_flags(delivery));
    if (packet) enet_host_broadcast(host, channel, packet);  // destroys the packet if nobody took it
}

void Host::flush() {
    if (ENetHost* host = as_host(host_)) enet_host_flush(host);
}

void Host::disconnect(PeerId peer, u32 user_data) {
    if (ENetPeer* p = peer_at(host_, peer)) enet_peer_disconnect_later(p, user_data);
}

void Host::disconnect_now(PeerId peer, u32 user_data) {
    if (ENetPeer* p = peer_at(host_, peer)) {
        enet_peer_disconnect_now(p, user_data);
        connected_[peer] = 0;
    }
}

void Host::close() {
    release_received();
    ENetHost* host = as_host(host_);
    if (!host) return;
    for (usize i = 0; i < host->peerCount; ++i) {
        if (host->peers[i].state != ENET_PEER_STATE_DISCONNECTED) enet_peer_disconnect_now(&host->peers[i], 0);
    }
    enet_host_flush(host);
    enet_host_destroy(host);
    host_ = nullptr;
    connected_.clear();
    port_ = 0;
    release_enet();
}

bool Host::is_connected(PeerId peer) const { return peer < connected_.size() && connected_[peer] != 0; }

u32 Host::connected_count() const {
    u32 n = 0;
    for (u8 c : connected_) n += c;
    return n;
}

void Host::connected_peers(Array<PeerId>& out) const {
    out.clear();
    for (usize i = 0; i < connected_.size(); ++i) {
        if (connected_[i]) out.push_back(static_cast<PeerId>(i));
    }
}

PeerStats Host::stats(PeerId peer) const {
    PeerStats s;
    const ENetPeer* p = peer_at(host_, peer);
    if (!p) return s;
    s.rtt_ms = p->roundTripTime;
    s.rtt_variance_ms = p->roundTripTimeVariance;
    s.packet_loss = p->packetLoss;
    return s;
}

u64 Host::bytes_sent() const {
    const ENetHost* host = as_host(host_);
    return host ? host->totalSentData : 0;
}

u64 Host::bytes_received() const {
    const ENetHost* host = as_host(host_);
    return host ? host->totalReceivedData : 0;
}

}  // namespace mce::net
