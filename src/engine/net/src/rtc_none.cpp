// RtcLinks without WebRTC (MCE_WITH_RTC off, and platforms without it): nothing opens, so RtcHost
// keeps every pair on the relay.

#include "rtc_links.h"

namespace mce::net {

struct RtcLinks::Impl {};

RtcLinks::RtcLinks() = default;
RtcLinks::~RtcLinks() = default;
bool RtcLinks::available() { return false; }
u32 RtcLinks::open(bool, StringView) { return 0; }
void RtcLinks::set_remote(u32, StringView, StringView) {}
void RtcLinks::add_candidate(u32, StringView, StringView) {}
bool RtcLinks::send(u32, bool, Span<const u8>) { return false; }
void RtcLinks::close(u32) {}
bool RtcLinks::next(RtcLinkEvent&) { return false; }

}  // namespace mce::net
