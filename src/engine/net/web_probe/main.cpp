// mce_relay_web_probe: the browser half of mce_relay's selftest (M-WEB, M-NET2 step 6). A web build
// only: two RelayClients over the browser's WebSocket (websocket_web.cpp) reach a native mce_relay,
// one makes a room, the other joins it by code, and a message crosses each way.
//
//   mce_relay -bind 127.0.0.1 -ws_port 7778 -udp_port 0
//   open mce_relay_web_probe.html?port=7778       logs "relay web probe: OK" (or FAILED) and exits
//
// scripts/web/probe_smoke.cjs runs it headless against a relay it starts (relay mode).

#include <emscripten.h>

#include <cstdio>
#include <cstring>

#include "mce/core.h"
#include "mce/net/net.h"

using namespace mce;

namespace {

// ?port=N from the page's address; 7778 (mce_relay's default) when absent.
EM_JS(int, probe_port, (), {
    const p = new URLSearchParams(location.search).get("port");
    return p ? Number(p) : 7778;
});

struct Member {
    net::WebSocketHost link;
    net::RelayClient room;
    Array<u8> last;
    u32 connected = 0;
    void pump() {
        for (net::Event ev; room.poll(ev);) {
            if (ev.kind == net::EventKind::Connected) ++connected;
            if (ev.kind == net::EventKind::Received) {
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

}  // namespace

int main() {
    log::init({.app_name = "mce_relay_web_probe", .level = log::Level::Info});
    const u16 port = static_cast<u16>(probe_port());
    Member host, guest;
    bool ok = host.link.open_client() && guest.link.open_client();
    if (ok) {
        host.room.start(host.link, host.link.connect("127.0.0.1", port));
        guest.room.start(guest.link, guest.link.connect("127.0.0.1", port));
        ok = host.room.create_room(2);
    }
    // The browser delivers socket events between tasks: sleep (ASYNCIFY) between polls.
    auto run_until = [&](auto&& done) {
        for (int i = 0; i < 1000; ++i) {  // about 10 s
            host.pump();
            guest.pump();
            if (done()) return true;
            emscripten_sleep(10);
        }
        return false;
    };
    ok = ok && run_until([&] { return host.room.state() == net::RelayClient::State::InRoom; });
    FixedString<8> code(host.room.room_code());
    std::printf("relay web probe: room %s made over WebSocket\n", ok ? code.c_str() : "-");
    ok = ok && guest.room.join_room(code.view());
    ok = ok && run_until([&] { return guest.room.state() == net::RelayClient::State::InRoom && host.connected == 1; });
    std::printf("relay web probe: the second client joined as member %u\n", ok ? guest.room.member_id() : 0u);
    ok = ok && guest.room.send(0, 1, bytes("hello host"), net::Delivery::Reliable);
    ok = ok && host.room.send(guest.room.member_id(), 0, bytes("hello guest"), net::Delivery::Sequenced);
    ok = ok && run_until([&] { return host.got("hello host") && guest.got("hello guest"); });
    std::printf("relay web probe: messages crossed both ways: %s\n", ok ? "yes" : "no");
    host.room.leave();
    guest.room.leave();
    std::printf("relay web probe: %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
