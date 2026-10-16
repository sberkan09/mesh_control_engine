// mce_rtc_web_probe: the browser half of the WebRTC gate (M-NET3 step 4). A web build only: an
// RtcHost over the browser's WebSocket reaches a native mce_relay, quick-plays into the room of the
// relay's echo member (a native RtcHost on libdatachannel), connects to it directly over the
// browser's RTCPeerConnection, and checks that messages come back over the direct link.
//
//   mce_relay -bind 127.0.0.1 -ws_port 7778 -echo_mode rtc_probe
//   open mce_rtc_web_probe.html?port=7778       logs "rtc web probe: OK" (or FAILED) and exits
//
// scripts/web/probe_smoke.cjs runs it headless against a relay it starts (rtc mode).

#include <emscripten.h>

#include <cstdio>
#include <cstring>

#include "mce/core.h"
#include "mce/net/net.h"

using namespace mce;

namespace {

EM_JS(int, probe_port, (), {
    const p = new URLSearchParams(location.search).get("port");
    return p ? Number(p) : 7778;
});

Span<const u8> bytes(StringView s) { return Span<const u8>(reinterpret_cast<const u8*>(s.data()), s.size()); }

}  // namespace

int main() {
    log::init({.app_name = "mce_rtc_web_probe", .level = log::Level::Info});
    const u16 port = static_cast<u16>(probe_port());
    net::WebSocketHost link;
    net::RtcHost room;
    bool ok = link.open_client();
    if (ok) {
        room.start(link, link.connect("127.0.0.1", port));
        ok = room.room().quick_play("rtc_probe", 2);
    }
    u32 connected = 0;
    Array<u8> reliable, unreliable;
    auto pump = [&] {
        for (net::Event ev; room.poll(ev);) {
            if (ev.kind == net::EventKind::Connected) ++connected;
            if (ev.kind != net::EventKind::Received) continue;
            Array<u8>& to = ev.channel == 0 ? reliable : unreliable;
            to.resize(ev.data.size());
            if (!ev.data.empty()) std::memcpy(to.data(), ev.data.data(), ev.data.size());
        }
        room.flush();
    };
    auto got = [](const Array<u8>& b, StringView text) {
        return b.size() == text.size() && std::memcmp(b.data(), text.data(), text.size()) == 0;
    };
    // The browser delivers socket and data-channel events between tasks: sleep (ASYNCIFY) between polls.
    auto run_until = [&](auto&& done, int steps) {
        for (int i = 0; i < steps; ++i) {
            pump();
            if (done()) return true;
            emscripten_sleep(10);
        }
        return false;
    };
    ok = ok && run_until([&] { return room.room().state() == net::RelayClient::State::InRoom && connected == 1; }, 1000);
    std::printf("rtc web probe: joined the echo member's room as member %u (WebRTC %s)\n",
                ok ? room.room().member_id() : 0u, net::RtcHost::direct_supported() ? "on" : "off");
    const net::PeerId echo = room.room().host_id();
    // The echo member offers (the lower id), so the pair is on the relay until its offer arrives.
    ok = ok && run_until([&] { return room.path(echo) == net::RtcPath::Direct; }, 1500);
    std::printf("rtc web probe: the pair is %s\n", net::rtc_path_name(room.path(echo)).data());
    ok = ok && room.path(echo) == net::RtcPath::Direct;
    ok = ok && room.send(echo, 0, bytes("ping"), net::Delivery::Reliable);
    ok = ok && run_until([&] { return got(reliable, "direct:ping"); }, 500);
    std::printf("rtc web probe: a Reliable message came back direct: %s\n", ok ? "yes" : "no");
    // Unreliable messages may be lost: send until one comes back.
    ok = ok && run_until([&] {
        room.send(echo, 1, bytes("pong"), net::Delivery::Unreliable);
        return got(unreliable, "direct:pong");
    }, 500);
    std::printf("rtc web probe: an Unreliable message came back direct: %s\n", ok ? "yes" : "no");
    std::printf("rtc web probe: %llu direct messages sent, %llu through the relay\n",
                static_cast<unsigned long long>(room.rtc_stats().direct_messages_sent),
                static_cast<unsigned long long>(room.rtc_stats().relay_messages_sent));
    room.room().leave();
    std::printf("rtc web probe: %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
