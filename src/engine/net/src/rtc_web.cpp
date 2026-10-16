// RtcLinks in a web build (M-NET3 step 4): the browser's RTCPeerConnection and RTCDataChannel. The
// links live in JavaScript (Module.mceRtc); their callbacks queue events there, and next() takes
// them one at a time. A link's remote steps (description, then candidates) run on one promise chain,
// so a candidate is never added before the description it belongs to.

#include <emscripten.h>

#include <cstring>

#include "rtc_links.h"

EM_JS_DEPS(mce_rtc, "$UTF8ToString,$stringToUTF8");

// clang-format off
EM_JS(int, mce_rtc_open, (int offerer, const char* server), {
    const R = Module.mceRtc || (Module.mceRtc = { links: new Map(), next: 1, queue: [] });
    if (typeof RTCPeerConnection === "undefined") return 0;
    const id = R.next++;
    const config = { iceServers: [] };
    const s = UTF8ToString(server);
    if (s) config.iceServers.push({ urls: s });
    let pc;
    try { pc = new RTCPeerConnection(config); } catch (e) { return 0; }
    const L = { pc: pc, r: null, u: null, open: false, closed: false, chain: Promise.resolve() };
    R.links.set(id, L);
    const enc = new TextEncoder();
    const push = (kind, bytes, mid, reliable) => {
        if (R.links.get(id) !== L) return;
        R.queue.push({ link: id, kind: kind, bytes: bytes || new Uint8Array(0), mid: mid || "", reliable: !!reliable });
    };
    L.fail = () => { if (!L.closed) { L.closed = true; push(3); } };
    const opened = () => {
        if (L.open || L.closed || !L.r || !L.u || L.r.readyState !== "open" || L.u.readyState !== "open") return;
        L.open = true;
        push(2);
    };
    const watch = (dc) => {
        dc.binaryType = "arraybuffer";
        dc.onopen = opened;
        dc.onclose = L.fail;
        dc.onmessage = (e) => { if (e.data instanceof ArrayBuffer) push(4, new Uint8Array(e.data), "", dc === L.r); };
    };
    pc.onicecandidate = (e) => {
        if (e.candidate && e.candidate.candidate) push(1, enc.encode(e.candidate.candidate), e.candidate.sdpMid || "");
    };
    pc.onconnectionstatechange = () => {
        const st = pc.connectionState;
        if (st === "failed" || st === "closed" || st === "disconnected") L.fail();
    };
    pc.ondatachannel = (e) => {
        const dc = e.channel;
        if (dc.label === "r" && !L.r) L.r = dc;
        else if (dc.label === "u" && !L.u) L.u = dc;
        else return;
        watch(dc);
        opened();
    };
    L.describe = () => push(0, enc.encode(pc.localDescription.sdp), pc.localDescription.type);
    if (offerer) {
        L.r = pc.createDataChannel("r", { ordered: true });
        L.u = pc.createDataChannel("u", { ordered: false, maxRetransmits: 0 });
        watch(L.r);
        watch(L.u);
        L.chain = L.chain.then(() => pc.createOffer()).then((o) => pc.setLocalDescription(o)).then(L.describe).catch(L.fail);
    }
    return id;
});

EM_JS(void, mce_rtc_remote, (int id, const char* type, const char* sdp, int size), {
    const L = Module.mceRtc && Module.mceRtc.links.get(id);
    if (!L) return;
    const desc = { type: UTF8ToString(type), sdp: UTF8ToString(sdp, size) };
    L.chain = L.chain.then(() => L.pc.setRemoteDescription(desc)).then(() => {
        if (desc.type === "offer") return L.pc.createAnswer().then((a) => L.pc.setLocalDescription(a)).then(L.describe);
    }).catch(L.fail);
});

EM_JS(void, mce_rtc_candidate, (int id, const char* candidate, int size, const char* mid), {
    const L = Module.mceRtc && Module.mceRtc.links.get(id);
    if (!L) return;
    const c = { candidate: UTF8ToString(candidate, size), sdpMid: UTF8ToString(mid) || "0" };
    L.chain = L.chain.then(() => L.pc.addIceCandidate(c)).catch(() => {});  // one bad candidate is not fatal
});

EM_JS(int, mce_rtc_send, (int id, int reliable, const unsigned char* data, int size), {
    const L = Module.mceRtc && Module.mceRtc.links.get(id);
    if (!L || !L.open || L.closed) return 0;
    const dc = reliable ? L.r : L.u;
    if (dc.readyState !== "open") return 0;
    try { dc.send(HEAPU8.slice(data, data + size)); } catch (e) { return 0; }
    return 1;
});

EM_JS(void, mce_rtc_close, (int id), {
    const R = Module.mceRtc;
    const L = R && R.links.get(id);
    if (!L) return;
    R.links.delete(id);
    R.queue = R.queue.filter((e) => e.link !== id);
    L.closed = true;
    try { if (L.r) L.r.close(); if (L.u) L.u.close(); L.pc.close(); } catch (e) {}
});

// The size of the next event's bytes, or -1 when there is none.
EM_JS(int, mce_rtc_peek, (), {
    const R = Module.mceRtc;
    return R && R.queue.length ? R.queue[0].bytes.length : -1;
});

// Takes the next event: info[0] link, info[1] kind, info[2] reliable; its bytes into buf.
EM_JS(void, mce_rtc_take, (int* info, unsigned char* buf, char* mid, int mid_size), {
    const e = Module.mceRtc.queue.shift();
    HEAP32[info >> 2] = e.link;
    HEAP32[(info >> 2) + 1] = e.kind;
    HEAP32[(info >> 2) + 2] = e.reliable ? 1 : 0;
    HEAPU8.set(e.bytes, buf);
    stringToUTF8(e.mid, mid, mid_size);
});
// clang-format on

namespace mce::net {

struct RtcLinks::Impl {
    Array<u32> links;
};

RtcLinks::RtcLinks() : impl_(mem::make_unique<Impl>(mem::heap())) {}

RtcLinks::~RtcLinks() {
    for (u32 l : impl_->links) mce_rtc_close(static_cast<int>(l));
}

bool RtcLinks::available() { return true; }

u32 RtcLinks::open(bool offerer, StringView ice_server) {
    FixedString<256> server(ice_server);
    const int id = mce_rtc_open(offerer ? 1 : 0, server.c_str());
    if (id <= 0) return 0;
    impl_->links.push_back(static_cast<u32>(id));
    return static_cast<u32>(id);
}

void RtcLinks::set_remote(u32 link, StringView type, StringView sdp) {
    FixedString<16> t(type);
    mce_rtc_remote(static_cast<int>(link), t.c_str(), sdp.data(), static_cast<int>(sdp.size()));
}

void RtcLinks::add_candidate(u32 link, StringView candidate, StringView mid) {
    if (candidate.empty()) return;
    FixedString<32> m(mid);
    mce_rtc_candidate(static_cast<int>(link), candidate.data(), static_cast<int>(candidate.size()), m.c_str());
}

bool RtcLinks::send(u32 link, bool reliable, Span<const u8> data) {
    return mce_rtc_send(static_cast<int>(link), reliable ? 1 : 0, data.data(), static_cast<int>(data.size())) != 0;
}

void RtcLinks::close(u32 link) {
    for (usize i = 0; i < impl_->links.size(); ++i) {
        if (impl_->links[i] != link) continue;
        impl_->links.erase_swap(i);
        mce_rtc_close(static_cast<int>(link));
        return;
    }
}

bool RtcLinks::next(RtcLinkEvent& out) {
    const int size = mce_rtc_peek();
    if (size < 0) return false;
    out.text.resize(static_cast<usize>(size));
    int info[3] = {};
    char mid[32] = {};
    mce_rtc_take(info, out.text.data(), mid, static_cast<int>(sizeof(mid)));
    out.link = static_cast<u32>(info[0]);
    out.kind = static_cast<RtcLinkEvent::Kind>(info[1]);
    out.reliable = info[2] != 0;
    out.mid.assign(StringView(mid));
    return true;
}

}  // namespace mce::net
