#include "mce/net/relay.h"

#include <cstring>

#include "mce/log.h"

namespace mce::net {
namespace {

// Client to relay.
constexpr u8 kCreate = 0x01;  // version, max_members
constexpr u8 kJoin = 0x02;    // version, code[6]
constexpr u8 kLeave = 0x03;
constexpr u8 kData = 0x04;  // to (member, 0xFF = everyone else), channel, delivery, payload
constexpr u8 kKick = 0x05;  // member (host only)
// Relay to client.
constexpr u8 kCreated = 0x81;       // member, code[6]
constexpr u8 kJoined = 0x82;        // member, code[6], members present (bit mask)
constexpr u8 kRefused = 0x83;       // reason
constexpr u8 kMemberJoined = 0x84;  // member
constexpr u8 kMemberLeft = 0x85;    // member
constexpr u8 kDataFrom = 0x86;      // from, channel, payload
constexpr u8 kClosed = 0x87;        // the room is gone (the host left, or this client was removed)

constexpr u8 kEveryone = 0xFF;
constexpr usize kDataHeader = 4;
constexpr usize kDataFromHeader = 3;

u64 client_key(u32 transport, PeerId peer) { return (u64(transport + 1) << 32) | peer; }
u32 key_transport(u64 key) { return static_cast<u32>(key >> 32) - 1; }
PeerId key_peer(u64 key) { return static_cast<PeerId>(key & 0xFFFFFFFFu); }

char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

// A code as a u64 (its six characters, upper-cased), 0 when it is not a valid code.
u64 pack_code(const char* text, usize size) {
    if (size != kRelayCodeLength) return 0;
    u64 v = 0;
    for (usize i = 0; i < size; ++i) {
        const char c = upper(text[i]);
        if (std::strchr(kRelayCodeAlphabet, c) == nullptr || c == '\0') return 0;
        v = (v << 8) | static_cast<u8>(c);
    }
    return v;
}

void unpack_code(u64 code, char out[kRelayCodeLength]) {
    for (usize i = 0; i < kRelayCodeLength; ++i) out[i] = static_cast<char>((code >> (8 * (kRelayCodeLength - 1 - i))) & 0xFF);
}

u8 link_channel(Delivery d) { return d == Delivery::Reliable ? 0 : 1; }

void append(Array<u8>& out, const void* data, usize size) {
    const usize at = out.size();
    out.resize(at + size);
    if (size) std::memcpy(out.data() + at, data, size);
}

}  // namespace

StringView relay_refusal_name(RelayRefusal r) {
    switch (r) {
        case RelayRefusal::None: return "none";
        case RelayRefusal::NoSuchRoom: return "no such room";
        case RelayRefusal::RoomFull: return "room full";
        case RelayRefusal::AlreadyInRoom: return "already in a room";
        case RelayRefusal::BadRequest: return "bad request";
        case RelayRefusal::TooManyRooms: return "too many rooms";
    }
    return "unknown";
}

// ---- RelayServer --------------------------------------------------------------------------------

void RelayServer::init(const RelayDesc& desc) {
    desc_ = desc;
    transports_.clear();
    clients_.clear();
    rooms_.clear();
    rng_ = desc.seed ? desc.seed : 1;
    stats_ = RelayStats();
}

u32 RelayServer::add_transport(Transport& transport) {
    transports_.push_back(&transport);
    return static_cast<u32>(transports_.size() - 1);
}

void RelayServer::update() {
    for (u32 t = 0; t < transports_.size(); ++t) {
        Transport& link = *transports_[t];
        for (Event ev; link.poll(ev);) {
            const u64 key = client_key(t, ev.peer);
            switch (ev.kind) {
                case EventKind::Connected: {
                    Client c;
                    c.transport = t;
                    c.peer = ev.peer;
                    clients_.insert_or_assign(key, c);
                    ++stats_.clients;
                    break;
                }
                case EventKind::Disconnected:
                    if (clients_.find(key)) {
                        leave(key, false);
                        clients_.erase(key);
                        --stats_.clients;
                    }
                    break;
                case EventKind::Received:
                    if (clients_.find(key)) receive(key, ev.data);
                    break;
            }
        }
    }
    for (Transport* t : transports_) t->flush();
}

bool RelayServer::room_members(StringView code, u32& members) const {
    const Room* room = rooms_.find(pack_code(code.data(), code.size()));
    if (!room) return false;
    members = 0;
    for (u64 m : room->members)
        if (m) ++members;
    return true;
}

void RelayServer::send_to(u64 key, u8 channel, Span<const u8> data, Delivery delivery) {
    transports_[key_transport(key)]->send(key_peer(key), channel, data, delivery);
}

void RelayServer::refuse(u64 key, RelayRefusal reason) {
    ++stats_.refusals;
    const u8 msg[2] = {kRefused, static_cast<u8>(reason)};
    send_to(key, 0, Span<const u8>(msg, 2), Delivery::Reliable);
}

u64 RelayServer::new_code() {
    for (;;) {
        char text[kRelayCodeLength];
        for (char& c : text) {
            rng_ ^= rng_ << 13;
            rng_ ^= rng_ >> 7;
            rng_ ^= rng_ << 17;
            c = kRelayCodeAlphabet[(rng_ >> 32) % (sizeof(kRelayCodeAlphabet) - 1)];
        }
        const u64 code = pack_code(text, kRelayCodeLength);
        if (!rooms_.find(code)) return code;
    }
}

void RelayServer::receive(u64 key, Span<const u8> data) {
    if (data.empty()) {
        ++stats_.dropped;
        return;
    }
    switch (data[0]) {
        case kCreate: create(key, data); break;
        case kJoin: join(key, data); break;
        case kLeave: leave(key, true); break;
        case kData: forward(key, data); break;
        case kKick: {
            const Client* c = clients_.find(key);
            Room* room = c && c->room ? rooms_.find(c->room) : nullptr;
            if (!room || c->member != 0 || data.size() != 2 || data[1] == 0 || data[1] >= kRelayMaxMembers || !room->members[data[1]]) {
                ++stats_.dropped;
                break;
            }
            const u64 kicked = room->members[data[1]];
            leave(kicked, true);
            break;
        }
        default: ++stats_.dropped; break;
    }
}

void RelayServer::create(u64 key, Span<const u8> data) {
    Client* c = clients_.find(key);
    if (data.size() != 3 || data[1] != kRelayVersion || data[2] < 2 || data[2] > kRelayMaxMembers) return refuse(key, RelayRefusal::BadRequest);
    if (c->room) return refuse(key, RelayRefusal::AlreadyInRoom);
    if (rooms_.size() >= desc_.max_rooms) return refuse(key, RelayRefusal::TooManyRooms);
    Room room;
    room.code = new_code();
    room.max_members = data[2];
    room.members[0] = key;
    rooms_.insert_or_assign(room.code, room);
    c->room = room.code;
    c->member = 0;
    ++stats_.rooms_created;
    ++stats_.rooms;
    u8 msg[2 + kRelayCodeLength] = {kCreated, 0};
    unpack_code(room.code, reinterpret_cast<char*>(msg + 2));
    send_to(key, 0, Span<const u8>(msg, sizeof(msg)), Delivery::Reliable);
}

void RelayServer::join(u64 key, Span<const u8> data) {
    Client* c = clients_.find(key);
    if (data.size() != 2 + kRelayCodeLength || data[1] != kRelayVersion) return refuse(key, RelayRefusal::BadRequest);
    if (c->room) return refuse(key, RelayRefusal::AlreadyInRoom);
    const u64 code = pack_code(reinterpret_cast<const char*>(data.data() + 2), kRelayCodeLength);
    Room* room = code ? rooms_.find(code) : nullptr;
    if (!room) return refuse(key, RelayRefusal::NoSuchRoom);
    u8 member = 0;
    u8 present = 0;
    for (u8 i = 0; i < room->max_members; ++i) {
        if (room->members[i]) present = static_cast<u8>(present | (1u << i));
        else if (member == 0) member = i;
    }
    if (member == 0) return refuse(key, RelayRefusal::RoomFull);
    room->members[member] = key;
    c->room = code;
    c->member = member;
    ++stats_.joins;
    u8 msg[3 + kRelayCodeLength] = {kJoined, member};
    unpack_code(code, reinterpret_cast<char*>(msg + 2));
    msg[2 + kRelayCodeLength] = present;
    send_to(key, 0, Span<const u8>(msg, sizeof(msg)), Delivery::Reliable);
    const u8 note[2] = {kMemberJoined, member};
    for (u8 i = 0; i < kRelayMaxMembers; ++i)
        if (room->members[i] && i != member) send_to(room->members[i], 0, Span<const u8>(note, 2), Delivery::Reliable);
}

void RelayServer::leave(u64 key, bool notify_self) {
    Client* c = clients_.find(key);
    if (!c || !c->room) return;
    Room* room = rooms_.find(c->room);
    const u64 code = c->room;
    const u8 member = c->member;
    c->room = 0;
    c->member = 0;
    if (notify_self) {
        const u8 msg[1] = {kClosed};
        send_to(key, 0, Span<const u8>(msg, 1), Delivery::Reliable);
    }
    if (!room) return;
    room->members[member] = 0;
    if (member == 0) {
        // The host left: the room closes.
        for (u64 other : room->members) {
            if (!other) continue;
            if (Client* oc = clients_.find(other)) {
                oc->room = 0;
                oc->member = 0;
            }
            const u8 msg[1] = {kClosed};
            send_to(other, 0, Span<const u8>(msg, 1), Delivery::Reliable);
        }
        rooms_.erase(code);
        --stats_.rooms;
        return;
    }
    const u8 note[2] = {kMemberLeft, member};
    for (u64 other : room->members)
        if (other) send_to(other, 0, Span<const u8>(note, 2), Delivery::Reliable);
}

void RelayServer::forward(u64 key, Span<const u8> data) {
    const Client* c = clients_.find(key);
    const Room* room = c && c->room ? rooms_.find(c->room) : nullptr;
    if (!room || data.size() < kDataHeader || data.size() > desc_.max_message || data[3] > static_cast<u8>(Delivery::Unreliable)) {
        ++stats_.dropped;
        return;
    }
    const u8 to = data[1];
    const Delivery delivery = static_cast<Delivery>(data[3]);
    scratch_.clear();
    const u8 head[kDataFromHeader] = {kDataFrom, c->member, data[2]};
    append(scratch_, head, sizeof(head));
    append(scratch_, data.data() + kDataHeader, data.size() - kDataHeader);
    const Span<const u8> out(scratch_.data(), scratch_.size());
    u32 sent = 0;
    for (u8 i = 0; i < kRelayMaxMembers; ++i) {
        if (!room->members[i] || i == c->member || (to != kEveryone && to != i)) continue;
        send_to(room->members[i], link_channel(delivery), out, delivery);
        ++sent;
    }
    if (sent == 0) ++stats_.dropped;
    stats_.forwarded += sent;
    stats_.forwarded_bytes += u64(sent) * (data.size() - kDataHeader);
}

// ---- RelayClient --------------------------------------------------------------------------------

void RelayClient::start(Transport& link, PeerId relay, const RelayClientDesc& desc) {
    link_ = &link;
    relay_ = relay;
    channels_ = desc.channels ? desc.channels : 1;
    state_ = State::Idle;
    refusal_ = RelayRefusal::None;
    code_.clear();
    member_ = kInvalidPeer;
    for (bool& p : present_) p = false;
    request_.clear();
    request_sent_ = false;
    inbox_.clear();
    inbox_head_ = 0;
}

bool RelayClient::send_request() {
    if (request_.empty() || request_sent_ || !link_ || !link_->is_connected(relay_)) return false;
    link_->send(relay_, 0, Span<const u8>(request_.data(), request_.size()), Delivery::Reliable);
    request_sent_ = true;
    return true;
}

bool RelayClient::create_room(u32 max_members) {
    if (!link_ || state_ == State::InRoom || state_ == State::Requesting) return false;
    if (max_members < 2 || max_members > kRelayMaxMembers) return false;
    request_.clear();
    const u8 msg[3] = {kCreate, kRelayVersion, static_cast<u8>(max_members)};
    append(request_, msg, sizeof(msg));
    request_sent_ = false;
    state_ = State::Requesting;
    refusal_ = RelayRefusal::None;
    send_request();
    return true;
}

bool RelayClient::join_room(StringView code) {
    if (!link_ || state_ == State::InRoom || state_ == State::Requesting) return false;
    if (pack_code(code.data(), code.size()) == 0) {
        state_ = State::Refused;
        refusal_ = RelayRefusal::NoSuchRoom;
        return false;
    }
    request_.clear();
    const u8 msg[2] = {kJoin, kRelayVersion};
    append(request_, msg, sizeof(msg));
    for (char ch : code) {
        const u8 u = static_cast<u8>(upper(ch));
        append(request_, &u, 1);
    }
    request_sent_ = false;
    state_ = State::Requesting;
    refusal_ = RelayRefusal::None;
    send_request();
    return true;
}

void RelayClient::push(EventKind kind, PeerId peer, u8 channel, const u8* data, usize size) {
    Queued q;
    q.event.kind = kind;
    q.event.peer = peer;
    q.event.channel = channel;
    append(q.bytes, data, size);
    inbox_.push_back(std::move(q));
}

void RelayClient::member_left(PeerId peer) {
    if (peer >= kRelayMaxMembers || !present_[peer]) return;
    present_[peer] = false;
    push(EventKind::Disconnected, peer, 0, nullptr, 0);
}

void RelayClient::close_room(State next) {
    for (PeerId p = 0; p < kRelayMaxMembers; ++p) member_left(p);
    state_ = next;
    member_ = kInvalidPeer;
    request_.clear();
    request_sent_ = false;
}

void RelayClient::leave() {
    if (!link_) return;
    if (state_ == State::InRoom && link_->is_connected(relay_)) {
        const u8 msg[1] = {kLeave};
        link_->send(relay_, 0, Span<const u8>(msg, 1), Delivery::Reliable);
    }
    if (state_ == State::InRoom || state_ == State::Requesting) close_room(State::Idle);
}

void RelayClient::handle(Span<const u8> d) {
    if (d.empty()) return;
    switch (d[0]) {
        case kCreated:
        case kJoined: {
            const usize need = d[0] == kCreated ? 2 + kRelayCodeLength : 3 + kRelayCodeLength;
            if (d.size() != need || state_ != State::Requesting || d[1] >= kRelayMaxMembers) return;
            member_ = d[1];
            code_.assign(StringView(reinterpret_cast<const char*>(d.data() + 2), kRelayCodeLength));
            state_ = State::InRoom;
            request_.clear();
            if (d[0] == kJoined) {
                const u8 present = d[2 + kRelayCodeLength];
                for (PeerId p = 0; p < kRelayMaxMembers; ++p) {
                    if ((present & (1u << p)) == 0 || p == member_) continue;
                    present_[p] = true;
                    push(EventKind::Connected, p, 0, nullptr, 0);
                }
            }
            break;
        }
        case kRefused:
            if (state_ != State::Requesting) return;
            refusal_ = d.size() == 2 ? static_cast<RelayRefusal>(d[1]) : RelayRefusal::BadRequest;
            state_ = State::Refused;
            request_.clear();
            break;
        case kMemberJoined:
            if (d.size() != 2 || state_ != State::InRoom || d[1] >= kRelayMaxMembers || d[1] == member_ || present_[d[1]]) return;
            present_[d[1]] = true;
            push(EventKind::Connected, d[1], 0, nullptr, 0);
            break;
        case kMemberLeft:
            if (d.size() == 2 && state_ == State::InRoom) member_left(d[1]);
            break;
        case kDataFrom:
            if (d.size() < kDataFromHeader || state_ != State::InRoom || d[1] >= kRelayMaxMembers || !present_[d[1]] || d[2] >= channels_)
                return;
            push(EventKind::Received, d[1], d[2], d.data() + kDataFromHeader, d.size() - kDataFromHeader);
            break;
        case kClosed:
            if (state_ == State::InRoom) close_room(State::Closed);
            break;
        default: break;
    }
}

bool RelayClient::poll(Event& out) {
    if (!link_) return false;
    if (inbox_head_ >= inbox_.size()) {
        inbox_.clear();
        inbox_head_ = 0;
        for (Event ev; link_->poll(ev);) {
            if (ev.peer != relay_) continue;
            if (ev.kind == EventKind::Connected) {
                send_request();
            } else if (ev.kind == EventKind::Disconnected) {
                if (state_ == State::InRoom || state_ == State::Requesting) {
                    MCE_LOG_WARN("net: lost the link to the relay");
                    close_room(State::Closed);
                }
            } else {
                handle(ev.data);
            }
        }
        if (inbox_.empty()) return false;
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

bool RelayClient::data(u8 to, u8 channel, Span<const u8> payload, Delivery delivery) {
    if (state_ != State::InRoom || channel >= channels_ || !link_->is_connected(relay_)) return false;
    scratch_.clear();
    const u8 head[kDataHeader] = {kData, to, channel, static_cast<u8>(delivery)};
    append(scratch_, head, sizeof(head));
    append(scratch_, payload.data(), payload.size());
    link_->send(relay_, link_channel(delivery), Span<const u8>(scratch_.data(), scratch_.size()), delivery);
    return true;
}

bool RelayClient::send(PeerId peer, u8 channel, Span<const u8> payload, Delivery delivery) {
    if (!is_connected(peer)) return false;
    if (!data(static_cast<u8>(peer), channel, payload, delivery)) return false;
    ++counters_.messages_sent;
    counters_.payload_bytes_sent += payload.size();
    return true;
}

void RelayClient::broadcast(u8 channel, Span<const u8> payload, Delivery delivery) {
    u32 others = 0;
    for (bool p : present_) others += p ? 1u : 0u;
    if (others == 0 || !data(kEveryone, channel, payload, delivery)) return;
    counters_.messages_sent += others;
    counters_.payload_bytes_sent += u64(others) * payload.size();
}

void RelayClient::flush() {
    if (link_) link_->flush();
}

void RelayClient::disconnect(PeerId peer, u32) {
    if (!is_connected(peer)) return;
    if (member_ == 0) {
        const u8 msg[2] = {kKick, static_cast<u8>(peer)};
        link_->send(relay_, 0, Span<const u8>(msg, 2), Delivery::Reliable);
        member_left(peer);
    } else if (peer == 0) {
        leave();
    }
}

bool RelayClient::is_connected(PeerId peer) const {
    return state_ == State::InRoom && peer < kRelayMaxMembers && present_[peer];
}

PeerStats RelayClient::stats(PeerId peer) const {
    if (!is_connected(peer)) return {};
    return link_->stats(relay_);
}

}  // namespace mce::net
