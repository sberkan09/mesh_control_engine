// WebSocketHost in a web build: the client side over the browser's WebSocket (emscripten's
// websocket API, linked with -lwebsocket.js). A browser cannot listen, so listen() fails. The
// browser handles the handshake, masking, pings and fragmentation; this file only maps its
// callbacks to Transport events. Messages are the native side's: [u8 channel][payload].
//
// Not built or run by local_ci.sh yet (no Emscripten there); the M-WEB track's builds compile it.

#include <emscripten/websocket.h>

#include <cstdio>
#include <cstring>

#include "mce/log.h"
#include "mce/net/websocket.h"

namespace mce::net {

struct WebSocketHost::Impl {
    enum class State : u8 { Free, Connecting, Connected };
    struct Peer {
        State state = State::Free;
        EMSCRIPTEN_WEBSOCKET_T socket = 0;
        Impl* owner = nullptr;
        PeerId id = kInvalidPeer;
    };
    struct Queued {
        Event event;
        Array<u8> bytes;
    };

    WebSocketDesc desc;
    Array<Peer> peers;
    Array<Queued> inbox;
    usize inbox_head = 0;
    Array<u8> current;
    Array<u8> scratch;

    void push(EventKind kind, PeerId peer, u8 channel, u32 user_data, const u8* data, usize size) {
        Queued q;
        q.event.kind = kind;
        q.event.peer = peer;
        q.event.channel = channel;
        q.event.user_data = user_data;
        q.bytes.resize(size);
        if (size) std::memcpy(q.bytes.data(), data, size);
        inbox.push_back(std::move(q));
    }

    void release(Peer& p) {
        if (p.socket) emscripten_websocket_delete(p.socket);
        p.socket = 0;
        p.state = State::Free;
    }

    static EM_BOOL on_open(int, const EmscriptenWebSocketOpenEvent*, void* user) {
        Peer& p = *static_cast<Peer*>(user);
        p.state = State::Connected;
        p.owner->push(EventKind::Connected, p.id, 0, 0, nullptr, 0);
        return EM_TRUE;
    }
    static EM_BOOL on_message(int, const EmscriptenWebSocketMessageEvent* e, void* user) {
        Peer& p = *static_cast<Peer*>(user);
        if (e->isText || e->numBytes == 0 || e->data[0] >= p.owner->desc.channels) return EM_TRUE;
        p.owner->push(EventKind::Received, p.id, e->data[0], 0, e->data + 1, e->numBytes - 1);
        return EM_TRUE;
    }
    static EM_BOOL on_close(int, const EmscriptenWebSocketCloseEvent*, void* user) {
        Peer& p = *static_cast<Peer*>(user);
        if (p.state != State::Free) p.owner->push(EventKind::Disconnected, p.id, 0, 0, nullptr, 0);
        p.owner->release(p);
        return EM_TRUE;
    }
    static EM_BOOL on_error(int, const EmscriptenWebSocketErrorEvent*, void* user) {
        Peer& p = *static_cast<Peer*>(user);
        if (p.state != State::Free) p.owner->push(EventKind::Disconnected, p.id, 0, 0, nullptr, 0);
        p.owner->release(p);
        return EM_TRUE;
    }
};

WebSocketHost::WebSocketHost() : impl_(mem::make_unique<Impl>(mem::heap())) {}

WebSocketHost::~WebSocketHost() { close(); }

bool WebSocketHost::listen(const WebSocketDesc&) {
    MCE_LOG_ERROR("net: a web build cannot listen for websocket connections");
    return false;
}

bool WebSocketHost::open_client(const WebSocketDesc& desc) {
    close();
    if (desc.channels == 0 || desc.max_peers == 0 || !emscripten_websocket_is_supported()) return false;
    impl_->desc = desc;
    impl_->peers.resize(desc.max_peers);
    for (u32 i = 0; i < desc.max_peers; ++i) {
        impl_->peers[i].owner = impl_.get();
        impl_->peers[i].id = i;
    }
    channels_ = desc.channels;
    open_ = true;
    return true;
}

PeerId WebSocketHost::connect(StringView host, u16 port, u32 user_data, StringView path) {
    if (!open_) return kInvalidPeer;
    PeerId slot = kInvalidPeer;
    for (u32 i = 0; i < impl_->peers.size(); ++i)
        if (impl_->peers[i].state == Impl::State::Free) {
            slot = i;
            break;
        }
    if (slot == kInvalidPeer) return kInvalidPeer;
    // A page served over https may only open wss://, so a host given as "wss://name" is used as is.
    FixedString<384> url;
    const bool has_scheme = host.substr(0, 5) == "ws://" || host.substr(0, 6) == "wss://";
    if (!has_scheme) url.append("ws://");
    url.append(host);
    char num[12];
    std::snprintf(num, sizeof(num), ":%u", static_cast<unsigned>(port));
    url.append(num);
    url.append(path.empty() ? StringView("/") : path);
    url.append(path.find('?') == StringView::npos ? "?mce_user=" : "&mce_user=");
    std::snprintf(num, sizeof(num), "%u", static_cast<unsigned>(user_data));
    url.append(num);
    EmscriptenWebSocketCreateAttributes attrs;
    emscripten_websocket_init_create_attributes(&attrs);
    attrs.url = url.c_str();
    const EMSCRIPTEN_WEBSOCKET_T ws = emscripten_websocket_new(&attrs);
    if (ws <= 0) return kInvalidPeer;
    Impl::Peer& p = impl_->peers[slot];
    p.socket = ws;
    p.state = Impl::State::Connecting;
    emscripten_websocket_set_onopen_callback(ws, &p, Impl::on_open);
    emscripten_websocket_set_onmessage_callback(ws, &p, Impl::on_message);
    emscripten_websocket_set_onclose_callback(ws, &p, Impl::on_close);
    emscripten_websocket_set_onerror_callback(ws, &p, Impl::on_error);
    return slot;
}

bool WebSocketHost::poll(Event& out) {
    if (!open_) return false;
    Impl& m = *impl_;
    if (m.inbox_head >= m.inbox.size()) {
        m.inbox.clear();
        m.inbox_head = 0;
        return false;
    }
    Impl::Queued& q = m.inbox[m.inbox_head++];
    m.current = std::move(q.bytes);
    out = q.event;
    out.data = Span<const u8>(m.current.data(), m.current.size());
    if (out.kind == EventKind::Received) {
        ++counters_.messages_received;
        counters_.payload_bytes_received += m.current.size();
    }
    return true;
}

bool WebSocketHost::send(PeerId peer, u8 channel, Span<const u8> data, Delivery) {
    if (!is_connected(peer) || channel >= channels_ || data.size() + 1 > impl_->desc.max_message) return false;
    Array<u8>& s = impl_->scratch;
    s.resize(data.size() + 1);
    s[0] = channel;
    if (!data.empty()) std::memcpy(s.data() + 1, data.data(), data.size());
    if (emscripten_websocket_send_binary(impl_->peers[peer].socket, s.data(), static_cast<u32>(s.size())) != EMSCRIPTEN_RESULT_SUCCESS)
        return false;
    ++counters_.messages_sent;
    counters_.payload_bytes_sent += data.size();
    return true;
}

void WebSocketHost::broadcast(u8 channel, Span<const u8> data, Delivery delivery) {
    if (!open_) return;
    for (u32 i = 0; i < impl_->peers.size(); ++i)
        if (impl_->peers[i].state == Impl::State::Connected) send(i, channel, data, delivery);
}

void WebSocketHost::flush() {}  // the browser sends at once

void WebSocketHost::disconnect(PeerId peer, u32 user_data) {
    if (!open_ || peer >= impl_->peers.size()) return;
    Impl::Peer& p = impl_->peers[peer];
    if (p.state == Impl::State::Free) return;
    const bool was_connected = p.state == Impl::State::Connected;
    emscripten_websocket_close(p.socket, 1000, "");
    impl_->release(p);
    if (was_connected) impl_->push(EventKind::Disconnected, peer, 0, user_data, nullptr, 0);
}

void WebSocketHost::close() {
    for (Impl::Peer& p : impl_->peers) {
        if (p.state == Impl::State::Free) continue;
        emscripten_websocket_close(p.socket, 1000, "");
        impl_->release(p);
    }
    impl_->peers.clear();
    impl_->inbox.clear();
    impl_->inbox_head = 0;
    open_ = false;
}

bool WebSocketHost::is_connected(PeerId peer) const {
    return open_ && peer < impl_->peers.size() && impl_->peers[peer].state == Impl::State::Connected;
}

PeerStats WebSocketHost::stats(PeerId) const { return {}; }  // the browser does not expose its pings

usize WebSocketHost::pending_bytes(PeerId peer) const {
    if (!is_connected(peer)) return 0;
    size_t n = 0;
    emscripten_websocket_get_buffered_amount(impl_->peers[peer].socket, &n);
    return n;
}

u32 WebSocketHost::peer_slots() const { return static_cast<u32>(impl_->peers.size()); }

}  // namespace mce::net
