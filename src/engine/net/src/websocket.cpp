// WebSocketHost over native TCP sockets (websocket_web.cpp is the browser's).

#include <chrono>
#include <cstring>

#include "mce/log.h"
#include "mce/net/websocket.h"
#include "socket.h"
#include "ws_crypto.h"

namespace mce::net {
namespace {

using detail::IoResult;
using detail::SocketHandle;

constexpr u8 kOpContinuation = 0x0;
constexpr u8 kOpText = 0x1;
constexpr u8 kOpBinary = 0x2;
constexpr u8 kOpClose = 0x8;
constexpr u8 kOpPing = 0x9;
constexpr u8 kOpPong = 0xA;

constexpr u16 kCloseNormal = 1000;
constexpr u16 kCloseProtocol = 1002;
constexpr u16 kCloseUnsupported = 1003;
constexpr u16 kCloseTooBig = 1009;

constexpr usize kMaxHandshake = 8192;

u64 now_us() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Microseconds from `since` to `now` (0 when `since` was stamped later within the same pass).
u64 elapsed(u64 now, u64 since) { return now > since ? now - since : 0; }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(StringView a, StringView b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

bool icontains(StringView haystack, StringView needle) {
    if (needle.size() > haystack.size()) return false;
    for (usize i = 0; i + needle.size() <= haystack.size(); ++i)
        if (iequals(haystack.substr(i, needle.size()), needle)) return true;
    return false;
}

StringView trim(StringView s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// The value of header `name` in an HTTP head ("" when absent).
StringView header(StringView head, StringView name) {
    usize pos = head.find('\n');
    while (pos != StringView::npos && pos + 1 < head.size()) {
        const usize start = pos + 1;
        const usize end = head.find('\n', start);
        const StringView line = head.substr(start, end == StringView::npos ? StringView::npos : end - start);
        const usize colon = line.find(':');
        if (colon != StringView::npos && iequals(trim(line.substr(0, colon)), name)) return trim(line.substr(colon + 1));
        pos = end;
    }
    return {};
}

u32 parse_u32(StringView s) {
    u64 v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') break;
        v = v * 10 + static_cast<u64>(c - '0');
        if (v > 0xFFFFFFFFull) return 0;
    }
    return static_cast<u32>(v);
}

void append(Array<u8>& out, const void* data, usize size) {
    const usize at = out.size();
    out.resize(at + size);
    if (size) std::memcpy(out.data() + at, data, size);
}

void append(Array<u8>& out, StringView s) { append(out, s.data(), s.size()); }

}  // namespace

struct WebSocketHost::Impl {
    enum class State : u8 { Free, TcpConnecting, ClientHandshake, ServerHandshake, Connected, Closing };
    struct Peer {
        State state = State::Free;
        SocketHandle sock = detail::invalid_socket();
        bool client = false;  // we connected: our frames are masked, the server's are not
        Array<u8> in;
        Array<u8> out;
        usize out_head = 0;
        Array<u8> message;  // a fragmented message being put together
        bool assembling = false;
        FixedString<32> expect_accept;
        FixedString<320> request;
        u64 started_us = 0;
        u64 heard_us = 0;
        u64 ping_us = 0;  // when the outstanding ping went out (0: none)
        u64 last_ping_us = 0;
        u32 rtt_ms = 0;
        u32 rtt_var_ms = 0;
        bool shut = false;  // Closing: the send side has been shut down
        bool eof = false;   // the other side closed; what it sent before is parsed first
    };
    struct Queued {
        Event event;
        Array<u8> bytes;
    };

    WebSocketDesc desc;
    SocketHandle listener = detail::invalid_socket();
    bool sockets = false;
    Array<Peer> peers;
    Array<Queued> inbox;
    usize inbox_head = 0;
    Array<u8> current;
    u64 rng = 0;

    u32 random() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return static_cast<u32>(rng >> 16);
    }

    void push(EventKind kind, PeerId peer, u8 channel, u32 user_data, const u8* data, usize size) {
        Queued q;
        q.event.kind = kind;
        q.event.peer = peer;
        q.event.channel = channel;
        q.event.user_data = user_data;
        append(q.bytes, data, size);
        inbox.push_back(std::move(q));
    }

    void reset(PeerId id) {
        Peer& p = peers[id];
        detail::close_socket(p.sock);
        p = Peer();
    }

    // Drops a connection; Disconnected is reported when the game knew it (or was waiting for it).
    void drop(PeerId id, bool report, u32 user_data = 0) {
        if (report) push(EventKind::Disconnected, id, 0, user_data, nullptr, 0);
        reset(id);
    }

    bool reported(const Peer& p) const {
        return p.state == State::Connected || (p.client && (p.state == State::TcpConnecting || p.state == State::ClientHandshake));
    }

    void frame(Peer& p, u8 op, const u8* a, usize a_size, const u8* b, usize b_size) {
        const usize size = a_size + b_size;
        u8 head[14];
        usize n = 0;
        head[n++] = static_cast<u8>(0x80 | op);
        const u8 mask_bit = p.client ? 0x80 : 0x00;
        if (size < 126) {
            head[n++] = static_cast<u8>(mask_bit | size);
        } else if (size <= 0xFFFF) {
            head[n++] = static_cast<u8>(mask_bit | 126);
            head[n++] = static_cast<u8>(size >> 8);
            head[n++] = static_cast<u8>(size);
        } else {
            head[n++] = static_cast<u8>(mask_bit | 127);
            for (int i = 7; i >= 0; --i) head[n++] = static_cast<u8>(static_cast<u64>(size) >> (8 * i));
        }
        u8 mask[4] = {};
        if (p.client) {
            const u32 m = random();
            std::memcpy(mask, &m, 4);
            std::memcpy(head + n, mask, 4);
            n += 4;
        }
        append(p.out, head, n);
        const usize at = p.out.size();
        append(p.out, a, a_size);
        append(p.out, b, b_size);
        if (p.client)
            for (usize i = 0; i < size; ++i) p.out[at + i] ^= mask[i & 3];
    }

    void close_frame(Peer& p, u16 code, u32 user_data) {
        const u8 payload[6] = {static_cast<u8>(code >> 8), static_cast<u8>(code), static_cast<u8>(user_data),
                               static_cast<u8>(user_data >> 8), static_cast<u8>(user_data >> 16), static_cast<u8>(user_data >> 24)};
        frame(p, kOpClose, payload, user_data ? 6 : 2, nullptr, 0);
    }

    // A protocol error from the other side: tell it, report Disconnected, and wind down.
    void fail(PeerId id, u16 code) {
        Peer& p = peers[id];
        if (reported(p)) push(EventKind::Disconnected, id, 0, 0, nullptr, 0);
        close_frame(p, code, 0);
        p.state = State::Closing;
        p.started_us = now_us();
    }

    void write(PeerId id) {
        Peer& p = peers[id];
        while (p.out_head < p.out.size()) {
            usize n = 0;
            const IoResult r = detail::send_some(p.sock, p.out.data() + p.out_head, p.out.size() - p.out_head, n);
            if (r == IoResult::Closed) {
                drop(id, reported(p));
                return;
            }
            p.out_head += n;
            if (r == IoResult::WouldBlock || n == 0) break;
        }
        if (p.out_head == p.out.size()) {
            p.out.clear();
            p.out_head = 0;
        } else if (p.out_head > 65536 && p.out_head * 2 > p.out.size()) {
            const usize rest = p.out.size() - p.out_head;
            std::memmove(p.out.data(), p.out.data() + p.out_head, rest);
            p.out.resize(rest);
            p.out_head = 0;
        }
    }

    // Reads what is there. A closed socket sets eof (finish() acts on it once the bytes before
    // it are parsed).
    void read(PeerId id) {
        Peer& p = peers[id];
        u8 buf[16384];
        while (!p.eof) {
            usize n = 0;
            const IoResult r = detail::recv_some(p.sock, buf, sizeof(buf), n);
            if (r == IoResult::Closed) {
                p.eof = true;
                break;
            }
            if (n == 0) break;
            p.heard_us = now_us();
            if (p.state != State::Closing) append(p.in, buf, n);
            if (p.in.size() > static_cast<usize>(desc.max_message) + kMaxHandshake + 14) break;  // parse first
        }
    }

    // After parsing: a peer whose socket closed is gone.
    void finish(PeerId id) {
        Peer& p = peers[id];
        if (p.state == State::Free || !p.eof) return;
        drop(id, reported(p));
    }

    void consume(Peer& p, usize n) {
        const usize rest = p.in.size() - n;
        if (rest) std::memmove(p.in.data(), p.in.data() + n, rest);
        p.in.resize(rest);
    }

    // The HTTP head at the front of p.in (through the blank line), or "" while incomplete.
    static StringView head_of(const Peer& p) {
        const StringView all(reinterpret_cast<const char*>(p.in.data()), p.in.size());
        const usize end = all.find("\r\n\r\n");
        return end == StringView::npos ? StringView() : all.substr(0, end + 4);
    }

    void server_handshake(PeerId id) {
        Peer& p = peers[id];
        const StringView head = head_of(p);
        if (head.empty()) {
            if (p.in.size() > kMaxHandshake) drop(id, false);
            return;
        }
        const StringView key = header(head, "Sec-WebSocket-Key");
        const bool ok = head.substr(0, 4) == "GET " && icontains(header(head, "Upgrade"), "websocket") &&
                        icontains(header(head, "Connection"), "upgrade") && !key.empty() && key.size() <= 64;
        if (!ok) {
            append(p.out, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            p.in.clear();
            p.state = State::Closing;
            return;
        }
        // The client's connect() user_data travels as ?mce_user=N on the request line.
        const StringView line = head.substr(0, head.find('\r'));
        u32 user_data = 0;
        const usize q = line.find("mce_user=");
        if (q != StringView::npos) user_data = parse_u32(line.substr(q + 9));
        const FixedString<32> accept = websocket_accept_key(key);
        append(p.out, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ");
        append(p.out, accept.view());
        append(p.out, "\r\n\r\n");
        consume(p, head.size());
        p.state = State::Connected;
        p.last_ping_us = now_us();
        push(EventKind::Connected, id, 0, user_data, nullptr, 0);
    }

    void client_handshake(PeerId id) {
        Peer& p = peers[id];
        const StringView head = head_of(p);
        if (head.empty()) {
            if (p.in.size() > kMaxHandshake) drop(id, true);
            return;
        }
        const bool ok = head.substr(0, 12) == "HTTP/1.1 101" && header(head, "Sec-WebSocket-Accept") == p.expect_accept.view();
        if (!ok) {
            MCE_LOG_WARN("net: websocket handshake refused");
            drop(id, true);
            return;
        }
        consume(p, head.size());
        p.state = State::Connected;
        p.last_ping_us = now_us();
        push(EventKind::Connected, id, 0, 0, nullptr, 0);
    }

    // Parses every whole frame in p.in. False when the peer is gone or winding down.
    bool frames(PeerId id) {
        for (;;) {
            Peer& p = peers[id];
            if (p.state != State::Connected) return false;
            const usize have = p.in.size();
            if (have < 2) return true;
            const u8* d = p.in.data();
            const bool fin = (d[0] & 0x80) != 0;
            const u8 op = d[0] & 0x0F;
            const bool masked = (d[1] & 0x80) != 0;
            u64 len = d[1] & 0x7F;
            usize at = 2;
            if ((d[0] & 0x70) != 0 || masked == p.client) {
                fail(id, kCloseProtocol);  // reserved bits, or a client frame unmasked / a server frame masked
                return false;
            }
            if (len == 126) {
                if (have < 4) return true;
                len = (u64(d[2]) << 8) | d[3];
                at = 4;
            } else if (len == 127) {
                if (have < 10) return true;
                len = 0;
                for (usize i = 0; i < 8; ++i) len = (len << 8) | d[2 + i];
                at = 10;
            }
            if (len > desc.max_message || p.message.size() + len > desc.max_message) {
                fail(id, kCloseTooBig);
                return false;
            }
            u8 mask[4] = {};
            if (masked) {
                if (have < at + 4) return true;
                std::memcpy(mask, d + at, 4);
                at += 4;
            }
            if (have < at + len) return true;
            u8* payload = p.in.data() + at;
            const usize size = static_cast<usize>(len);
            if (masked)
                for (usize i = 0; i < size; ++i) payload[i] ^= mask[i & 3];

            if (op >= 0x8) {
                if (!fin || size > 125) {
                    fail(id, kCloseProtocol);
                    return false;
                }
                if (op == kOpClose) {
                    u32 user_data = 0;
                    if (size >= 6)
                        user_data = u32(payload[2]) | (u32(payload[3]) << 8) | (u32(payload[4]) << 16) | (u32(payload[5]) << 24);
                    push(EventKind::Disconnected, id, 0, user_data, nullptr, 0);
                    close_frame(p, kCloseNormal, 0);
                    p.in.clear();
                    p.state = State::Closing;
                    p.started_us = now_us();
                    return false;
                }
                if (op == kOpPing) {
                    frame(p, kOpPong, payload, size, nullptr, 0);
                } else if (op == kOpPong && size == 8 && p.ping_us != 0) {
                    u64 sent = 0;
                    std::memcpy(&sent, payload, 8);
                    if (sent == p.ping_us) {
                        const u32 rtt = static_cast<u32>((now_us() - sent) / 1000);
                        if (p.rtt_ms == 0 && p.rtt_var_ms == 0) {
                            p.rtt_ms = rtt;
                            p.rtt_var_ms = rtt / 2;
                        } else {
                            const u32 diff = rtt > p.rtt_ms ? rtt - p.rtt_ms : p.rtt_ms - rtt;
                            p.rtt_var_ms = (p.rtt_var_ms * 3 + diff) / 4;
                            p.rtt_ms = (p.rtt_ms * 7 + rtt) / 8;
                        }
                        p.ping_us = 0;
                    }
                } else if (op != kOpPong) {
                    fail(id, kCloseProtocol);
                    return false;
                }
            } else {
                if (op == kOpText) {
                    fail(id, kCloseUnsupported);
                    return false;
                }
                if ((op == kOpContinuation) != p.assembling || (op != kOpContinuation && op != kOpBinary)) {
                    fail(id, kCloseProtocol);
                    return false;
                }
                append(p.message, payload, size);
                p.assembling = !fin;
                if (fin) {
                    if (p.message.empty() || p.message[0] >= channels()) {
                        fail(id, kCloseProtocol);
                        return false;
                    }
                    push(EventKind::Received, id, p.message[0], 0, p.message.data() + 1, p.message.size() - 1);
                    p.message.clear();
                }
            }
            consume(p, at + size);
        }
    }

    u8 channels() const { return desc.channels; }

    void service() {
        const u64 now = now_us();
        if (listener != detail::invalid_socket()) {
            for (;;) {
                const SocketHandle s = detail::accept_tcp(listener);
                if (s == detail::invalid_socket()) break;
                PeerId slot = kInvalidPeer;
                for (u32 i = 0; i < peers.size(); ++i)
                    if (peers[i].state == State::Free) {
                        slot = i;
                        break;
                    }
                if (slot == kInvalidPeer) {
                    usize n = 0;
                    static constexpr char kFull[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
                    detail::send_some(s, reinterpret_cast<const u8*>(kFull), sizeof(kFull) - 1, n);
                    detail::close_socket(s);
                    continue;
                }
                Peer& p = peers[slot];
                p = Peer();
                p.sock = s;
                p.state = State::ServerHandshake;
                p.started_us = now;
                p.heard_us = now;
            }
        }
        const u64 timeout = u64(desc.timeout_ms) * 1000;
        for (u32 id = 0; id < peers.size(); ++id) {
            Peer& p = peers[id];
            switch (p.state) {
                case State::Free:
                    break;
                case State::TcpConnecting: {
                    const detail::ConnectState c = detail::connect_state(p.sock);
                    if (c == detail::ConnectState::Failed || elapsed(now, p.started_us) > timeout) {
                        drop(id, true);
                        break;
                    }
                    if (c == detail::ConnectState::Pending) break;
                    append(p.out, p.request.view());
                    p.state = State::ClientHandshake;
                    write(id);
                    break;
                }
                case State::ClientHandshake:
                case State::ServerHandshake:
                    if (elapsed(now, p.started_us) > timeout) {
                        drop(id, p.client);
                        break;
                    }
                    write(id);
                    if (p.state == State::Free) break;
                    read(id);
                    if (p.state == State::ClientHandshake) client_handshake(id);
                    else server_handshake(id);
                    if (p.state == State::Connected) frames(id);  // anything that came after the head
                    finish(id);
                    if (p.state != State::Free) write(id);
                    break;
                case State::Connected:
                    read(id);
                    frames(id);
                    finish(id);
                    if (p.state == State::Connected) {
                        if (elapsed(now, p.heard_us) > timeout) {
                            MCE_LOG_WARN("net: websocket peer {} timed out", id);
                            drop(id, true);
                            break;
                        }
                        if (p.ping_us == 0 && elapsed(now, p.last_ping_us) >= u64(desc.ping_interval_ms) * 1000) {
                            p.ping_us = now;
                            p.last_ping_us = now;
                            frame(p, kOpPing, reinterpret_cast<const u8*>(&p.ping_us), 8, nullptr, 0);
                        }
                    }
                    if (p.state != State::Free) write(id);
                    break;
                case State::Closing:
                    write(id);
                    if (p.state == State::Free) break;
                    if (p.out_head == p.out.size() && !p.shut) {
                        detail::shutdown_send(p.sock);
                        p.shut = true;
                    }
                    read(id);
                    if (p.eof || elapsed(now, p.started_us) > timeout) reset(id);  // the other side closed too: done
                    break;
            }
        }
    }
};

WebSocketHost::WebSocketHost() : impl_(mem::make_unique<Impl>(mem::heap())) {}

WebSocketHost::~WebSocketHost() {
    close();
}

bool WebSocketHost::listen(const WebSocketDesc& desc) {
    close();
    if (desc.channels == 0 || desc.max_peers == 0) {
        MCE_LOG_ERROR("net: invalid websocket description (channels {}, max_peers {})", desc.channels, desc.max_peers);
        return false;
    }
    if (!detail::sockets_acquire()) return false;
    impl_->sockets = true;
    u16 bound = 0;
    impl_->listener = detail::listen_tcp(desc.bind_address.c_str(), desc.port, bound);
    if (impl_->listener == detail::invalid_socket()) {
        close();
        return false;
    }
    impl_->desc = desc;
    impl_->peers.resize(desc.max_peers);
    impl_->rng = now_us() | 1;
    port_ = bound;
    channels_ = desc.channels;
    open_ = true;
    MCE_LOG_INFO("net: websocket listening on port {} ({} peers, {} channels)", port_, desc.max_peers, desc.channels);
    return true;
}

bool WebSocketHost::open_client(const WebSocketDesc& desc) {
    close();
    if (desc.channels == 0 || desc.max_peers == 0) return false;
    if (!detail::sockets_acquire()) return false;
    impl_->sockets = true;
    impl_->desc = desc;
    impl_->peers.resize(desc.max_peers);
    impl_->rng = now_us() | 1;
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
    if (slot == kInvalidPeer) {
        MCE_LOG_ERROR("net: no free websocket slot to connect to {}:{}", host, port);
        return kInvalidPeer;
    }
    const FixedString<128> name(host);
    const SocketHandle s = detail::connect_tcp(name.c_str(), port);
    if (s == detail::invalid_socket()) return kInvalidPeer;
    Impl::Peer& p = impl_->peers[slot];
    p = Impl::Peer();
    p.sock = s;
    p.client = true;
    p.state = Impl::State::TcpConnecting;
    p.started_us = now_us();
    p.heard_us = p.started_us;
    u8 nonce[16];
    for (u32 i = 0; i < 16; i += 4) {
        const u32 r = impl_->random();
        std::memcpy(nonce + i, &r, 4);
    }
    const FixedString<64> key = detail::base64(nonce, sizeof(nonce));
    p.expect_accept = websocket_accept_key(key.view());
    char port_text[8];
    char user_text[12];
    auto digits = [](u32 v, char* out) {
        char tmp[12];
        usize n = 0;
        do {
            tmp[n++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        for (usize i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
        out[n] = '\0';
    };
    digits(port, port_text);
    digits(user_data, user_text);
    FixedString<320>& r = p.request;
    r.assign("GET ");
    r.append(path.empty() ? StringView("/") : path);
    r.append(path.find('?') == StringView::npos ? "?mce_user=" : "&mce_user=");
    r.append(user_text);
    r.append(" HTTP/1.1\r\nHost: ");
    r.append(host);
    r.append(":");
    r.append(port_text);
    r.append("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ");
    r.append(key.view());
    r.append("\r\nSec-WebSocket-Version: 13\r\n\r\n");
    if (r.truncated()) {
        impl_->reset(slot);
        return kInvalidPeer;
    }
    return slot;
}

bool WebSocketHost::poll(Event& out) {
    if (!open_) return false;
    Impl& m = *impl_;
    if (m.inbox_head >= m.inbox.size()) {
        m.inbox.clear();
        m.inbox_head = 0;
        m.service();
        if (m.inbox.empty()) return false;
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
    ++counters_.messages_sent;
    counters_.payload_bytes_sent += data.size();
    impl_->frame(impl_->peers[peer], kOpBinary, &channel, 1, data.data(), data.size());
    return true;
}

void WebSocketHost::broadcast(u8 channel, Span<const u8> data, Delivery delivery) {
    if (!open_) return;
    for (u32 i = 0; i < impl_->peers.size(); ++i)
        if (impl_->peers[i].state == Impl::State::Connected) send(i, channel, data, delivery);
}

void WebSocketHost::flush() {
    if (!open_) return;
    for (u32 i = 0; i < impl_->peers.size(); ++i) {
        const Impl::State s = impl_->peers[i].state;
        if (s == Impl::State::Connected || s == Impl::State::Closing || s == Impl::State::ServerHandshake) impl_->write(i);
    }
}

void WebSocketHost::disconnect(PeerId peer, u32 user_data) {
    if (!open_ || peer >= impl_->peers.size()) return;
    Impl::Peer& p = impl_->peers[peer];
    if (p.state == Impl::State::Connected) {
        impl_->close_frame(p, kCloseNormal, user_data);
        impl_->push(EventKind::Disconnected, peer, 0, user_data, nullptr, 0);
        p.state = Impl::State::Closing;
        p.started_us = now_us();
        impl_->write(peer);
    } else if (p.state != Impl::State::Free && p.state != Impl::State::Closing) {
        impl_->reset(peer);
    }
}

void WebSocketHost::close() {
    Impl& m = *impl_;
    for (u32 i = 0; i < m.peers.size(); ++i) {
        Impl::Peer& p = m.peers[i];
        if (p.state == Impl::State::Connected) {
            m.close_frame(p, kCloseNormal, 0);
            m.write(i);
        }
        if (m.peers[i].state != Impl::State::Free) m.reset(i);
    }
    m.peers.clear();
    detail::close_socket(m.listener);
    m.listener = detail::invalid_socket();
    m.inbox.clear();
    m.inbox_head = 0;
    if (m.sockets) detail::sockets_release();
    m.sockets = false;
    open_ = false;
    port_ = 0;
}

bool WebSocketHost::is_connected(PeerId peer) const {
    return open_ && peer < impl_->peers.size() && impl_->peers[peer].state == Impl::State::Connected;
}

PeerStats WebSocketHost::stats(PeerId peer) const {
    PeerStats s;
    if (!is_connected(peer)) return s;
    s.rtt_ms = impl_->peers[peer].rtt_ms;
    s.rtt_variance_ms = impl_->peers[peer].rtt_var_ms;
    return s;
}

usize WebSocketHost::pending_bytes(PeerId peer) const {
    if (!open_ || peer >= impl_->peers.size()) return 0;
    const Impl::Peer& p = impl_->peers[peer];
    return p.out.size() - p.out_head;
}

u32 WebSocketHost::peer_slots() const { return static_cast<u32>(impl_->peers.size()); }

}  // namespace mce::net
