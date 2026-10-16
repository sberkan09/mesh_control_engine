#pragma once
// The WebRTC layer under RtcHost (M-NET3 step 4): peer connections, each with two data channels,
// "r" (reliable, ordered) and "u" (unordered, no retransmits). Three builds of it:
//   rtc_native.cpp  libdatachannel (ICE, DTLS, SCTP) where MCE_WITH_RTC is on (Linux with OpenSSL)
//   rtc_web.cpp     the browser's RTCPeerConnection in a web build
//   rtc_none.cpp    nothing: available() is false and every pair stays on the relay
// RtcHost does the signalling (through the relay) and the framing; this layer only moves SDP,
// candidates and messages. Events come out of next() on the thread that calls it.

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/memory/allocator.h"
#include "mce/types.h"

namespace mce::net {

struct RtcLinkEvent {
    enum class Kind : u8 {
        Description,  // text: the local SDP; mid: "offer" or "answer"
        Candidate,    // text: a local ICE candidate; mid: its media id
        Open,         // both channels are open
        Closed,       // a channel closed or the connection failed (after Open or before it)
        Message,      // text: the bytes; reliable: which channel
    };
    Kind kind = Kind::Message;
    u32 link = 0;
    bool reliable = false;
    Array<u8> text;
    FixedString<32> mid;
};

class RtcLinks {
public:
    RtcLinks();
    ~RtcLinks();
    RtcLinks(const RtcLinks&) = delete;
    RtcLinks& operator=(const RtcLinks&) = delete;

    // Whether this build has WebRTC at all.
    static bool available();
    // A new connection; the offerer makes the channels and its offer comes out as a Description.
    // ice_server: "stun:host:port", or empty for host candidates only. 0 when it could not be made.
    u32 open(bool offerer, StringView ice_server);
    // The other side's description ("offer" or "answer") and candidates, in the order they came.
    void set_remote(u32 link, StringView type, StringView sdp);
    void add_candidate(u32 link, StringView candidate, StringView mid);
    // False when the link is not open or the channel refused the message.
    bool send(u32 link, bool reliable, Span<const u8> data);
    // No events come from the link afterwards.
    void close(u32 link);
    bool next(RtcLinkEvent& out);

private:
    struct Impl;
    mem::UniquePtr<Impl> impl_;
};

}  // namespace mce::net
