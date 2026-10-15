#include "mce/net/net_stats.h"

#include <cstdio>

#include "mce/ecs/dev_stats.h"

namespace mce::net {

void NetStatsMeter::update(const Transport& transport, f64 dt_seconds) {
    const TransportCounters& now = transport.counters();
    if (!started_) {
        start_ = now;
        started_ = true;
    }
    rates_.peers = 0;
    rates_.rtt_ms = 0;
    rates_.rtt_variance_ms = 0;
    rates_.loss = 0.0f;
    Array<PeerId> peers;
    transport.connected_peers(peers);
    for (PeerId p : peers) {
        const PeerStats s = transport.stats(p);
        ++rates_.peers;
        if (s.rtt_ms >= rates_.rtt_ms) {
            rates_.rtt_ms = s.rtt_ms;
            rates_.rtt_variance_ms = s.rtt_variance_ms;
        }
        const f32 loss = static_cast<f32>(s.packet_loss) / 65536.0f;
        if (loss > rates_.loss) rates_.loss = loss;
    }
    rates_.dropped = now.messages_dropped;

    elapsed_ += dt_seconds > 0.0 ? dt_seconds : 0.0;
    if (elapsed_ < window_s_) return;
    const f64 inv = 1.0 / elapsed_;
    rates_.up_bytes_per_s =
        static_cast<f32>(static_cast<f64>(now.payload_bytes_sent - start_.payload_bytes_sent) * inv);
    rates_.down_bytes_per_s =
        static_cast<f32>(static_cast<f64>(now.payload_bytes_received - start_.payload_bytes_received) * inv);
    rates_.up_messages_per_s = static_cast<f32>(static_cast<f64>(now.messages_sent - start_.messages_sent) * inv);
    rates_.down_messages_per_s =
        static_cast<f32>(static_cast<f64>(now.messages_received - start_.messages_received) * inv);
    start_ = now;
    elapsed_ = 0.0;
}

void NetStatsMeter::format(FixedString<160>& out) const {
    char line[160];
    std::snprintf(
        line, sizeof line, "NET %u peer%s  RTT %u ms +-%u  LOSS %.1f%%  UP %.1f KB/s  DOWN %.1f KB/s  DROP %llu",
        rates_.peers, rates_.peers == 1 ? "" : "s", rates_.rtt_ms, rates_.rtt_variance_ms,
        static_cast<double>(rates_.loss) * 100.0, static_cast<double>(rates_.up_bytes_per_s) / 1024.0,
        static_cast<double>(rates_.down_bytes_per_s) / 1024.0, static_cast<unsigned long long>(rates_.dropped));
    out.assign(StringView(line));
}

void NetStatsMeter::publish(World& world) const {
    DevStatsLines* lines = world.try_resource<DevStatsLines>();
    if (!lines) lines = &world.set_resource(DevStatsLines{});
    FixedString<160> text;
    format(text);
    lines->set("net", text.view());
}

}  // namespace mce::net
