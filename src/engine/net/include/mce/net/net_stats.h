#pragma once
// NetStatsMeter — a Transport's numbers per second for the dev overlay (M-NET2 step 1): peers,
// round trip, loss, bandwidth and dropped messages. update() once per frame; publish() writes the
// "net" line of the world's DevStatsLines, which the stock player draws under its stats line (F3).
//
//   meter.update(host, frame_seconds);
//   meter.publish(world);   // NET 1 peer  RTT 152 ms ±30  LOSS 5.0%  UP 3.2 KB/s  DOWN 41.0 KB/s  DROP 12

#include "mce/ecs/world.h"
#include "mce/net/transport.h"

namespace mce::net {

struct NetRates {
    u32 peers = 0;
    u32 rtt_ms = 0;           // the slowest connected peer
    u32 rtt_variance_ms = 0;  // that peer's
    f32 loss = 0.0f;          // the worst connected peer's estimate, 0..1
    f32 up_bytes_per_s = 0.0f;
    f32 down_bytes_per_s = 0.0f;
    f32 up_messages_per_s = 0.0f;
    f32 down_messages_per_s = 0.0f;
    u64 dropped = 0;  // since the transport opened
};

class NetStatsMeter {
public:
    // Rates are averaged over `window_s` seconds and refreshed when a window closes.
    explicit NetStatsMeter(f64 window_s = 1.0) : window_s_(window_s > 0.0 ? window_s : 1.0) {}

    void update(const Transport& transport, f64 dt_seconds);
    const NetRates& rates() const { return rates_; }

    // "NET 1 peer  RTT 152 ms ±30  LOSS 5.0%  UP 3.2 KB/s  DOWN 41.0 KB/s  DROP 12"
    void format(FixedString<160>& out) const;
    // Sets DevStatsLines "net" (adding the resource if the world has none).
    void publish(World& world) const;

private:
    f64 window_s_;
    f64 elapsed_ = 0.0;
    TransportCounters start_;
    bool started_ = false;
    NetRates rates_;
};

}  // namespace mce::net
