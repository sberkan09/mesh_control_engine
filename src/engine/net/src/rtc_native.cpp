// RtcLinks over libdatachannel (M-NET3 step 4): ICE (libjuice), DTLS (OpenSSL) and SCTP (usrsctp)
// for WebRTC data channels, natively. libdatachannel calls back on its own threads; every callback
// only queues an event under the mutex, and next() hands them out on the caller's thread.
//
// Built where MCE_WITH_RTC is on (cmake/Dependencies.cmake); libdatachannel's headers stay in this
// file (the WebRTC wall in scripts/ci/check_walls.py).

#include <rtc/rtc.h>

#include <cstring>
#include <mutex>

#include "mce/log.h"
#include "rtc_links.h"

namespace mce::net {

namespace {

std::mutex g_init_mutex;
u32 g_users = 0;

void log_callback(rtcLogLevel level, const char* message) {
    if (level <= RTC_LOG_ERROR) MCE_LOG_WARN("net: webrtc: {}", message);
}

}  // namespace

struct RtcLinks::Impl {
    struct Link {
        Impl* owner = nullptr;
        u32 id = 0;
        int pc = -1;
        int reliable = -1;    // data channel "r"
        int unreliable = -1;  // data channel "u"
        bool reliable_open = false;
        bool unreliable_open = false;
        bool open_sent = false;
        bool closed = false;  // Closed was queued
    };

    std::mutex mutex;  // events, and the links' channel ids and flags
    Array<RtcLinkEvent> events;
    usize head = 0;
    Array<mem::UniquePtr<Link>> links;
    u32 next_id = 1;

    void push(RtcLinkEvent&& ev) { events.push_back(std::move(ev)); }

    static void push_text(Link& l, RtcLinkEvent::Kind kind, const char* text, StringView mid) {
        RtcLinkEvent ev;
        ev.kind = kind;
        ev.link = l.id;
        const usize n = std::strlen(text);
        ev.text.resize(n);
        if (n) std::memcpy(ev.text.data(), text, n);
        ev.mid.assign(mid);
        l.owner->push(std::move(ev));
    }

    static void push_state(Link& l, RtcLinkEvent::Kind kind) {
        RtcLinkEvent ev;
        ev.kind = kind;
        ev.link = l.id;
        l.owner->push(std::move(ev));
    }

    // Callbacks (libdatachannel's threads).
    static void on_description(int, const char* sdp, const char* type, void* user) {
        Link& l = *static_cast<Link*>(user);
        std::lock_guard<std::mutex> lock(l.owner->mutex);
        push_text(l, RtcLinkEvent::Kind::Description, sdp, type);
    }
    static void on_candidate(int, const char* candidate, const char* mid, void* user) {
        Link& l = *static_cast<Link*>(user);
        std::lock_guard<std::mutex> lock(l.owner->mutex);
        push_text(l, RtcLinkEvent::Kind::Candidate, candidate, mid ? StringView(mid) : StringView());
    }
    static void on_state(int, rtcState state, void* user) {
        Link& l = *static_cast<Link*>(user);
        if (state != RTC_FAILED && state != RTC_CLOSED && state != RTC_DISCONNECTED) return;
        std::lock_guard<std::mutex> lock(l.owner->mutex);
        if (l.closed) return;
        l.closed = true;
        push_state(l, RtcLinkEvent::Kind::Closed);
    }
    // Channel callbacks carry the link too: a channel's user pointer is its link.
    static void on_open(int dc, void* user) {
        Link& l = *static_cast<Link*>(user);
        std::lock_guard<std::mutex> lock(l.owner->mutex);
        if (dc == l.reliable) l.reliable_open = true;
        if (dc == l.unreliable) l.unreliable_open = true;
        if (!l.reliable_open || !l.unreliable_open || l.closed || l.open_sent) return;
        l.open_sent = true;
        push_state(l, RtcLinkEvent::Kind::Open);
    }
    static void on_closed(int, void* user) {
        Link& l = *static_cast<Link*>(user);
        std::lock_guard<std::mutex> lock(l.owner->mutex);
        if (l.closed) return;
        l.closed = true;
        push_state(l, RtcLinkEvent::Kind::Closed);
    }
    static void on_message(int dc, const char* message, int size, void* user) {
        if (size < 0) return;  // text: never sent
        Link& l = *static_cast<Link*>(user);
        std::lock_guard<std::mutex> lock(l.owner->mutex);
        RtcLinkEvent ev;
        ev.kind = RtcLinkEvent::Kind::Message;
        ev.link = l.id;
        ev.reliable = dc == l.reliable;
        ev.text.resize(static_cast<usize>(size));
        if (size) std::memcpy(ev.text.data(), message, static_cast<usize>(size));
        l.owner->push(std::move(ev));
    }
    static void watch_channel(Link& l, int dc) {
        rtcSetUserPointer(dc, &l);
        rtcSetOpenCallback(dc, on_open);
        rtcSetClosedCallback(dc, on_closed);
        rtcSetMessageCallback(dc, on_message);
    }
    // The answerer's channels arrive from the offerer.
    static void on_channel(int, int dc, void* user) {
        Link& l = *static_cast<Link*>(user);
        char label[4] = {};
        rtcGetDataChannelLabel(dc, label, sizeof(label));
        {
            std::lock_guard<std::mutex> lock(l.owner->mutex);
            if (std::strcmp(label, "r") == 0 && l.reliable < 0) l.reliable = dc;
            else if (std::strcmp(label, "u") == 0 && l.unreliable < 0) l.unreliable = dc;
            else return;
        }
        watch_channel(l, dc);
        // It may have opened before its callbacks were set.
        if (rtcIsOpen(dc)) on_open(dc, &l);
    }

    Link* find(u32 id) {
        for (auto& l : links)
            if (l->id == id) return l.get();
        return nullptr;
    }
    static void release(Link& l) {
        // Deleting the connection waits for its callbacks and closes its channels.
        if (l.reliable >= 0) rtcDeleteDataChannel(l.reliable);
        if (l.unreliable >= 0) rtcDeleteDataChannel(l.unreliable);
        if (l.pc >= 0) rtcDeletePeerConnection(l.pc);
        l.pc = l.reliable = l.unreliable = -1;
    }
};

RtcLinks::RtcLinks() : impl_(mem::make_unique<Impl>(mem::heap())) {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_users++ == 0) {
        rtcInitLogger(RTC_LOG_ERROR, log_callback);
        rtcPreload();
    }
}

RtcLinks::~RtcLinks() {
    for (auto& l : impl_->links) Impl::release(*l);
    impl_->links.clear();
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (--g_users == 0) rtcCleanup();
}

bool RtcLinks::available() { return true; }

u32 RtcLinks::open(bool offerer, StringView ice_server) {
    FixedString<256> server(ice_server);
    const char* servers[1] = {server.c_str()};
    rtcConfiguration config;
    std::memset(&config, 0, sizeof(config));
    config.iceServers = server.empty() ? nullptr : servers;
    config.iceServersCount = server.empty() ? 0 : 1;
    config.disableAutoNegotiation = false;

    auto owned = mem::make_unique<Impl::Link>(mem::heap());
    Impl::Link& l = *owned;
    l.owner = impl_.get();
    l.id = impl_->next_id++;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->links.push_back(std::move(owned));
    }
    l.pc = rtcCreatePeerConnection(&config);
    if (l.pc < 0) {
        close(l.id);
        return 0;
    }
    rtcSetUserPointer(l.pc, &l);
    rtcSetLocalDescriptionCallback(l.pc, Impl::on_description);
    rtcSetLocalCandidateCallback(l.pc, Impl::on_candidate);
    rtcSetStateChangeCallback(l.pc, Impl::on_state);
    rtcSetDataChannelCallback(l.pc, Impl::on_channel);
    if (offerer) {
        rtcDataChannelInit init;
        std::memset(&init, 0, sizeof(init));
        const int reliable = rtcCreateDataChannelEx(l.pc, "r", &init);
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0;
        const int unreliable = rtcCreateDataChannelEx(l.pc, "u", &init);
        if (reliable < 0 || unreliable < 0) {
            if (reliable >= 0) rtcDeleteDataChannel(reliable);
            if (unreliable >= 0) rtcDeleteDataChannel(unreliable);
            close(l.id);
            return 0;
        }
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            l.reliable = reliable;
            l.unreliable = unreliable;
        }
        Impl::watch_channel(l, reliable);
        Impl::watch_channel(l, unreliable);
    }
    return l.id;
}

void RtcLinks::set_remote(u32 link, StringView type, StringView sdp) {
    Impl::Link* l = impl_->find(link);
    if (!l || l->pc < 0) return;
    FixedString<16> t(type);
    Array<char> text;
    text.resize(sdp.size() + 1);
    if (!sdp.empty()) std::memcpy(text.data(), sdp.data(), sdp.size());
    text[sdp.size()] = '\0';
    if (rtcSetRemoteDescription(l->pc, text.data(), t.c_str()) < 0) MCE_LOG_WARN("net: webrtc: a bad remote description");
}

void RtcLinks::add_candidate(u32 link, StringView candidate, StringView mid) {
    Impl::Link* l = impl_->find(link);
    if (!l || l->pc < 0 || candidate.empty()) return;
    FixedString<512> c(candidate);
    FixedString<32> m(mid);
    rtcAddRemoteCandidate(l->pc, c.c_str(), m.empty() ? nullptr : m.c_str());
}

bool RtcLinks::send(u32 link, bool reliable, Span<const u8> data) {
    Impl::Link* l = impl_->find(link);
    if (!l) return false;
    int dc = -1;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (l->closed || !l->reliable_open || !l->unreliable_open) return false;
        dc = reliable ? l->reliable : l->unreliable;
    }
    if (data.size() > 0x7fffffff) return false;
    return rtcSendMessage(dc, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size())) >= 0;
}

void RtcLinks::close(u32 link) {
    for (usize i = 0; i < impl_->links.size(); ++i) {
        if (impl_->links[i]->id != link) continue;
        Impl::release(*impl_->links[i]);
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->links.erase(i);
        // Its queued events go with it.
        Array<RtcLinkEvent> kept;
        for (usize e = impl_->head; e < impl_->events.size(); ++e)
            if (impl_->events[e].link != link) kept.push_back(std::move(impl_->events[e]));
        impl_->events = std::move(kept);
        impl_->head = 0;
        return;
    }
}

bool RtcLinks::next(RtcLinkEvent& out) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->head >= impl_->events.size()) {
        impl_->events.clear();
        impl_->head = 0;
        return false;
    }
    out = std::move(impl_->events[impl_->head++]);
    return true;
}

}  // namespace mce::net
