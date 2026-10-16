// mce_relay: rooms for players who cannot accept connections (M-NET2 step 6, mce/net/relay.h).
//
//   mce_relay                                   UDP on 7777, WebSocket on 7778, every interface
//   mce_relay -udp_port 9000 -ws_port 0         no WebSocket
//   mce_relay -bind 127.0.0.1 -seconds 60       loopback only, for a minute
//   mce_relay -selftest 1                       a native and a WebSocket client meet in a room, then exit
//   mce_relay -echo_mode rtc_probe              also a member that quick-plays "rtc_probe" rooms for 2,
//                                               connects to whoever joins over WebRTC and echoes back
//                                               what it gets (the browser probe's native peer, M-NET3)
//
// Every -stats seconds it logs clients, rooms and traffic. It holds no game state: a room lives as
// long as its host is connected, or, made with host migration, as long as anyone is in it.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "mce/core.h"
#include "mce/net/net.h"

using namespace mce;

namespace {

MCE_CVAR(cv_udp_port, "udp_port", 7777, "UDP (ENet) port for native clients; 0 = none", CVarFlags::None);
MCE_CVAR(cv_ws_port, "ws_port", 7778, "WebSocket port for web clients; 0 = none", CVarFlags::None);
MCE_CVAR(cv_bind, "bind", "", "Address to bind (empty = every interface)", CVarFlags::None);
MCE_CVAR(cv_max_rooms, "max_rooms", 256, "Rooms open at once at most", CVarFlags::None);
MCE_CVAR(cv_max_clients, "max_clients", 256, "Connections per transport at most", CVarFlags::None);
MCE_CVAR(cv_seconds, "seconds", 0, "Stop after this long (0 = run until killed)", CVarFlags::None);
MCE_CVAR(cv_stats, "stats", 30, "Log the counters every this many seconds (0 = never)", CVarFlags::None);
MCE_CVAR(cv_selftest, "selftest", 0, "Run a loopback room with a native and a WebSocket client, then exit", CVarFlags::None);
MCE_CVAR(cv_echo_mode, "echo_mode", "", "Also run an echoing RtcHost member in quick-play rooms of this mode", CVarFlags::None);

using Clock = std::chrono::steady_clock;

f64 seconds_since(Clock::time_point t) { return std::chrono::duration<f64>(Clock::now() - t).count(); }

void log_stats(const net::RelayStats& s) {
    MCE_LOG_INFO("relay: {} clients, {} rooms ({} created, {} joins, {} quick plays, {} host migrations, {} refused), "
                 "{} messages / {} bytes forwarded, {} dropped",
                 s.clients, s.rooms, s.rooms_created, s.joins, s.quick_plays, s.host_migrations, s.refusals, s.forwarded,
                 s.forwarded_bytes, s.dropped);
}

struct Member {
    net::RelayClient room;
    Array<u8> last;
    u32 received = 0;
    u32 connected = 0;
    u32 disconnected = 0;
    void pump() {
        for (net::Event ev; room.poll(ev);) {
            if (ev.kind == net::EventKind::Connected) ++connected;
            if (ev.kind == net::EventKind::Disconnected) ++disconnected;
            if (ev.kind == net::EventKind::Received) {
                ++received;
                last.resize(ev.data.size());
                if (!ev.data.empty()) std::memcpy(last.data(), ev.data.data(), ev.data.size());
            }
        }
    }
    bool got(StringView text) const {
        return last.size() == text.size() && std::memcmp(last.data(), text.data(), text.size()) == 0;
    }
};

Span<const u8> bytes(StringView s) { return Span<const u8>(reinterpret_cast<const u8*>(s.data()), s.size()); }

int selftest() {
    net::RelayServer relay;
    relay.init();
    net::HostDesc udp;
    udp.bind_address = "127.0.0.1";
    net::Host udp_host;
    net::WebSocketDesc wsd;
    wsd.bind_address = "127.0.0.1";
    net::WebSocketHost ws_host;
    if (!udp_host.listen(udp) || !ws_host.listen(wsd)) return 2;
    relay.add_transport(udp_host);
    relay.add_transport(ws_host);

    // The host plays a native build over UDP; the guest a web build over WebSocket.
    net::Host host_link;
    net::WebSocketHost guest_link;
    if (!host_link.open_client() || !guest_link.open_client()) return 2;
    Member host, guest, third;
    net::Host third_link;
    host.room.start(host_link, host_link.connect("127.0.0.1", udp_host.bound_port()));
    guest.room.start(guest_link, guest_link.connect("127.0.0.1", ws_host.bound_port()));
    host.room.create_room(2);

    const auto start = Clock::now();
    auto run_until = [&](auto&& done) {
        while (!done()) {
            relay.update();
            host.pump();
            guest.pump();
            third.pump();
            host.room.flush();
            guest.room.flush();
            third.room.flush();
            if (seconds_since(start) > 10.0) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    };
    bool ok = run_until([&] { return host.room.state() == net::RelayClient::State::InRoom; });
    FixedString<8> code(host.room.room_code());
    std::printf("mce_relay selftest: room %s made by the native client (UDP)\n", code.c_str());
    ok = ok && guest.room.join_room(code.view());
    ok = ok && run_until([&] { return guest.room.state() == net::RelayClient::State::InRoom && host.connected == 1; });
    std::printf("  the web client (WebSocket) joined as member %u\n", ok ? guest.room.member_id() : 0u);
    ok = ok && guest.room.send(0, 1, bytes("hello host"), net::Delivery::Reliable);
    ok = ok && host.room.send(guest.room.member_id(), 0, bytes("hello guest"), net::Delivery::Sequenced);
    ok = ok && run_until([&] { return host.got("hello host") && guest.got("hello guest"); });
    std::printf("  messages crossed both ways: %s\n", ok ? "yes" : "no");
    // A full room refuses a third member.
    ok = ok && third_link.open_client();
    third.room.start(third_link, third_link.connect("127.0.0.1", udp_host.bound_port()));
    ok = ok && third.room.join_room(code.view());
    ok = ok && run_until([&] { return third.room.state() == net::RelayClient::State::Refused; });
    ok = ok && third.room.refusal() == net::RelayRefusal::RoomFull;
    std::printf("  a third client was refused: %s\n", ok ? "room full" : "no");
    // The host leaving closes the room for the guest.
    host.room.leave();
    ok = ok && run_until([&] { return guest.room.state() == net::RelayClient::State::Closed && relay.stats().rooms == 0; });
    std::printf("  the host left and the room closed: %s\n", ok ? "yes" : "no");
    log_stats(relay.stats());
    std::printf("mce_relay selftest: %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}

// The -echo_mode member: an RtcHost over UDP to this relay that holds a quick-play room for 2 and
// sends every message back to its sender, on the channel it came on (channel 0 reliable, others
// unreliable), prefixed with how it arrived ("direct:" or "relay:").
struct EchoMember {
    net::Host link;
    net::RtcHost room;
    net::RtcPath paths[net::kRelayMaxMembers] = {};
    FixedString<net::kRelayModeLength + 1> mode;
    Array<u8> reply;

    bool start(u16 port, StringView m) {
        mode.assign(m);
        if (!link.open_client()) return false;
        room.start(link, link.connect("127.0.0.1", port));
        return room.room().quick_play(mode.view(), 2);
    }
    void pump() {
        for (net::Event ev; room.poll(ev);) {
            if (ev.kind == net::EventKind::Connected) MCE_LOG_INFO("relay: echo member: member {} joined", ev.peer);
            if (ev.kind == net::EventKind::Disconnected) MCE_LOG_INFO("relay: echo member: member {} left", ev.peer);
            if (ev.kind != net::EventKind::Received) continue;
            const bool direct = room.path(ev.peer) == net::RtcPath::Direct;
            const StringView how = direct ? StringView("direct:") : StringView("relay:");
            reply.resize(how.size() + ev.data.size());
            std::memcpy(reply.data(), how.data(), how.size());
            if (!ev.data.empty()) std::memcpy(reply.data() + how.size(), ev.data.data(), ev.data.size());
            room.send(ev.peer, ev.channel, Span<const u8>(reply.data(), reply.size()),
                      ev.channel == 0 ? net::Delivery::Reliable : net::Delivery::Unreliable);
        }
        for (net::PeerId p = 0; p < net::kRelayMaxMembers; ++p) {
            const net::RtcPath now = room.path(p);
            if (now != paths[p]) MCE_LOG_INFO("relay: echo member: member {} is {}", p, net::rtc_path_name(now));
            paths[p] = now;
        }
        // Back in a room when the last one closed (a host left it) or a request was refused.
        const auto state = room.room().state();
        if (state == net::RelayClient::State::Refused || state == net::RelayClient::State::Closed)
            room.room().quick_play(mode.view(), 2);
        room.flush();
    }
};

}  // namespace

int main(int argc, char** argv) {
    log::init({.app_name = "mce_relay", .level = log::Level::Info});
    console::parse_command_line(argc, argv);
    if (cv_selftest.get_int() != 0) return selftest();

    net::RelayDesc desc;
    desc.max_rooms = static_cast<u32>(std::max(1, cv_max_rooms.get_int()));
    desc.seed = static_cast<u64>(Clock::now().time_since_epoch().count());
    net::RelayServer relay;
    relay.init(desc);

    const u32 max_clients = static_cast<u32>(std::max(1, cv_max_clients.get_int()));
    net::Host udp;
    net::WebSocketHost ws;
    if (cv_udp_port.get_int() > 0) {
        net::HostDesc d;
        d.bind_address = cv_bind.get_string();
        d.port = static_cast<u16>(cv_udp_port.get_int());
        d.max_peers = max_clients;
        if (!udp.listen(d)) return 1;
        relay.add_transport(udp);
    }
    if (cv_ws_port.get_int() > 0) {
        net::WebSocketDesc d;
        d.bind_address = cv_bind.get_string();
        d.port = static_cast<u16>(cv_ws_port.get_int());
        d.max_peers = max_clients;
        if (!ws.listen(d)) return 1;
        relay.add_transport(ws);
    }
    if (!udp.is_open() && !ws.is_open()) {
        MCE_LOG_ERROR("relay: no port to listen on (-udp_port and -ws_port are both 0)");
        return 2;
    }
    MCE_LOG_INFO("relay: ready (UDP {}, WebSocket {})", udp.bound_port(), ws.bound_port());
    EchoMember echo;
    const bool echoing = !cv_echo_mode.get_string().empty();
    if (echoing) {
        if (!udp.is_open() || !echo.start(udp.bound_port(), cv_echo_mode.get_string())) {
            MCE_LOG_ERROR("relay: the echo member needs the UDP port and a valid mode");
            return 2;
        }
        MCE_LOG_INFO("relay: echo member in \"{}\" rooms (WebRTC {})", cv_echo_mode.get_string(),
                     net::RtcHost::direct_supported() ? "on" : "off: relay only");
    }

    const auto start = Clock::now();
    auto last_stats = start;
    const f64 limit = static_cast<f64>(std::max(0, cv_seconds.get_int()));
    const f64 every = static_cast<f64>(std::max(0, cv_stats.get_int()));
    while (limit == 0.0 || seconds_since(start) < limit) {
        relay.update();
        if (echoing) echo.pump();
        if (every > 0.0 && seconds_since(last_stats) >= every) {
            log_stats(relay.stats());
            last_stats = Clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    log_stats(relay.stats());
    return 0;
}
