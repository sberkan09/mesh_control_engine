#pragma once
// The arena client (M-NET3 step 5): the part of a player that is not a window or a bot. It mirrors
// the server's snapshots (interpolated through a jitter buffer), predicts its own player with the
// shared step and reconciles it on every acknowledgement, and fires shots stamped with the server
// tick it was drawing. Input comes from outside: a keyboard, or bot_input().

#include "arena.h"

namespace arena {

// A shot's line on screen: from where the shooter was drawn to the first wall (or the range).
struct Beam {
    math::vec2 from, to;
    u32 ticks = 0;  // left to draw
    u32 owner = 0;  // net id
};

// Where a ray from `o` along `d` (unit) first meets a wall box, or `range`.
inline f32 ray_to_wall(const Map& map, math::vec2 o, math::vec2 d, f32 range) {
    f32 best = range;
    for (const Box& b : map.walls) {
        f32 t0 = 0.0f, t1 = best;
        const f32 lo[2] = {b.center.x - b.half.x, b.center.y - b.half.y}, hi[2] = {b.center.x + b.half.x, b.center.y + b.half.y};
        const f32 po[2] = {o.x, o.y}, pd[2] = {d.x, d.y};
        bool hit = true;
        for (int a = 0; a < 2 && hit; ++a) {
            if (std::fabs(pd[a]) < 1e-9f) {
                hit = po[a] >= lo[a] && po[a] <= hi[a];
                continue;
            }
            f32 ta = (lo[a] - po[a]) / pd[a], tb = (hi[a] - po[a]) / pd[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::fmax(t0, ta);
            t1 = std::fmin(t1, tb);
            hit = t0 <= t1;
        }
        if (hit && t0 < best) best = t0;
    }
    return best;
}

inline f32 yaw_of(const math::quat& q) { return 2.0f * std::atan2(q.z, q.w); }

class Client {
public:
    World world;
    net::Transport* net = nullptr;
    net::RelayClient* room = nullptr;  // quick play: the server is the room's host
    net::ReplicationClient replication;
    net::Predictor<Input, State> predictor;
    Map map;
    net::PeerId server = net::kInvalidPeer;
    u32 own_id = 0;  // our player's net id, from WELCOME
    bool connected = false;
    u64 tick = 0;
    u32 snapshots = 0;
    u32 deltas = 0;  // of those, delta-coded
    u64 applied_hash = 0;
    u32 shots_sent = 0;
    Array<Beam> beams;

    void start(net::Transport& transport, net::PeerId to_server, net::RelayClient* via_room = nullptr) {
        net = &transport;
        server = to_server;
        room = via_room;
        world.set_resource<Client*>(this);
        replication.set_interpolation(net::InterpolationDesc{});
        predictor.init(net::PredictionDesc{}, step_player, &map);
        net::ReplicationHooks hooks;
        hooks.user = this;
        hooks.read_extra = [](void* user, Commands&, Entity e, net::ByteReader& in) {
            Client& c = *static_cast<Client*>(user);
            if (c.own_id == 0 || e != c.replication.find(c.own_id)) return;
            const u32 seq = in.read_u32();
            if (in.ok()) {
                c.ack_ = seq;
                c.has_ack_ = true;
            }
        };
        replication.set_hooks(hooks);
        world.add_system(Phase::FixedUpdate_Pre, "net_apply", [](Frame& f) {
            Client& c = *f.resource<Client*>();
            if (c.has_pending_) {
                const net::ApplyResult r = c.replication.apply(f.commands(), f.world(), c.pending_.data());
                if (r.ok) {
                    ++c.snapshots;
                    c.applied_hash = c.replication.content_hash();
                    if (r.delta) ++c.deltas;
                }
                c.has_pending_ = false;
            }
            c.predict(f);
            c.replication.update(f.commands(), f.world(), f.dt);  // draws the render time
        });
    }

    // Our player's mirrored health (null before it appears).
    const ArenaHealth* health() {
        const Entity e = own_id ? replication.find(own_id) : Entity::null;
        return world.alive(e) ? world.try_get<ArenaHealth>(e) : nullptr;
    }
    const LocalTransform* own_transform() {
        const Entity e = own_id ? replication.find(own_id) : Entity::null;
        return world.alive(e) ? world.try_get<LocalTransform>(e) : nullptr;
    }
    bool alive() {
        const ArenaHealth* h = health();
        return h && h->hp > 0;
    }

    // The server tick this client is drawing the other players at (what a shot is stamped with).
    f64 view_tick() {
        f64 view = static_cast<f64>(replication.last_tick());
        world.query<const net::NetId>().each([&](const net::NetId& n) {
            if (n.kind == kKindPlayer && n.id != own_id) view = replication.drawn_tick(n.id);
        });
        return view;
    }

    // One tick: `wish` is what the player holds; `fire` shoots along wish.aim. input = false only
    // resends unacknowledged inputs (a run settling).
    void step(const Input& wish, bool fire, bool input = true) {
        receive();
        if (connected && predictor.active()) {
            if (input) {
                Input in = wish;
                if (!alive()) in.move_x = in.move_y = 0.0f;  // dead players stand where they fell
                predictor.push(in);
            }
            send_inputs();
        }
        world.tick(kDt, ++tick);
        map.rebuild(world);
        if (input && fire && connected && alive() && tick >= fire_ready_) shoot(wish.aim);
        update_beams();
        net->flush();
    }

private:
    void predict(Frame& f) {
        const LocalTransform* server_t = own_id ? replication.latest_transform(own_id) : nullptr;
        if (!server_t) return;
        if (!predictor.active()) predictor.reset(State{*server_t});
        else if (has_ack_) predictor.reconcile(ack_, State{*server_t});
        has_ack_ = false;
        predictor.smooth(f.dt);
        f.commands().set(replication.find(own_id), predictor.displayed());
    }

    void shoot(f32 aim) {
        const f64 view = view_tick();
        u64 view_bits = 0;
        std::memcpy(&view_bits, &view, sizeof view_bits);
        net::ByteWriter shot;
        shot.write_u8(kMsgShot);
        shot.write_u64(view_bits);
        shot.write_f32(aim);
        net->send(server, kChannelControl, shot.data(), net::Delivery::Reliable);
        ++shots_sent;
        fire_ready_ = tick + kFireTicks;
        if (const LocalTransform* t = own_transform()) add_beam(own_id, {t->position.x, t->position.y}, aim);
    }

    void add_beam(u32 owner, math::vec2 from, f32 aim) {
        const math::vec2 d{std::cos(aim), std::sin(aim)};
        const f32 len = ray_to_wall(map, from, d, kShotRange);
        beams.push_back(Beam{from, {from.x + d.x * len, from.y + d.y * len}, 6, owner});
    }

    // Other players' shots: their fired counter went up.
    void update_beams() {
        for (usize i = 0; i < beams.size();) {
            if (beams[i].ticks-- <= 1) beams.erase(i);
            else ++i;
        }
        world.query<const net::NetId, const ArenaHealth, const LocalTransform>().each(
            [&](const net::NetId& n, const ArenaHealth& h, const LocalTransform& t) {
                if (n.id == own_id) return;
                u32* seen = nullptr;
                for (auto& s : seen_shots_)
                    if (s.first == n.id) seen = &s.second;
                if (!seen) {
                    seen_shots_.push_back({n.id, h.shots});  // the first sight: whatever was fired before is history
                    return;
                }
                if (h.shots != *seen) add_beam(n.id, {t.position.x, t.position.y}, yaw_of(t.rotation));
                *seen = h.shots;
            });
    }

    void send_inputs() {
        if (replication.last_tick() != 0) {
            net::ByteWriter ack;
            ack.write_u8(kMsgAck);
            replication.write_ack(ack);
            net->send(server, kChannelInput, ack.data(), net::Delivery::Sequenced);
        }
        net::ByteWriter packet;
        packet.write_u8(kMsgInput);
        if (predictor.write_inputs(packet)) net->send(server, kChannelInput, packet.data(), net::Delivery::Sequenced);
    }

    void receive() {
        for (net::Event ev; net->poll(ev);) {
            // Through a room the server is its host, and the other players are peers too.
            if (server == net::kInvalidPeer && room && room->state() == net::RelayClient::State::InRoom) server = room->host_id();
            if (ev.peer != server) continue;
            if (ev.kind == net::EventKind::Connected) {
                connected = true;
                const u8 join[1] = {kMsgJoin};
                net->send(server, kChannelControl, Span<const u8>(join, 1), net::Delivery::Reliable);
            } else if (ev.kind == net::EventKind::Disconnected) {
                connected = false;
                own_id = 0;
                predictor.stop();
                replication.reset(world.commands(), world);
            } else if (ev.kind == net::EventKind::Received && ev.channel == kChannelControl) {
                net::ByteReader r(ev.data);
                if (r.read_u8() == kMsgWelcome) {
                    const u32 id = r.read_u32();
                    if (r.ok()) {
                        own_id = id;
                        replication.set_predicted(id);
                    }
                }
            } else if (ev.kind == net::EventKind::Received && ev.channel == kChannelSnapshot) {
                pending_.clear();  // only the newest snapshot of the tick matters
                pending_.append(ev.data);
                has_pending_ = true;
            }
        }
    }

    net::ByteWriter pending_;
    bool has_pending_ = false;
    u32 ack_ = 0;
    bool has_ack_ = false;
    u64 fire_ready_ = 0;
    Array<std::pair<u32, u32>> seen_shots_;
};

// A bot's hands: it circles the arena at its own pace, aims at the nearest living enemy where it
// draws it (so its shots need the server's rewind) and fires whenever its gun is ready.
inline Input bot_input(Client& c, u32 index, u64 tick, bool& fire) {
    Input in;
    const f32 a = static_cast<f32>(index) * 1.7f + static_cast<f32>(tick) * 0.013f * (index % 2 ? -1.0f : 1.0f);
    in.move_x = std::cos(a);
    in.move_y = std::sin(a * 1.3f);
    in.aim = a;
    fire = false;
    const LocalTransform* me = c.own_transform();
    if (!me) return in;
    f32 best = 1e9f;
    c.world.query<const net::NetId, const ArenaHealth, const LocalTransform>().each(
        [&](const net::NetId& n, const ArenaHealth& h, const LocalTransform& t) {
            if (n.kind != kKindPlayer || n.id == c.own_id || h.hp == 0) return;
            const f32 dx = t.position.x - me->position.x, dy = t.position.y - me->position.y;
            const f32 d2 = dx * dx + dy * dy;
            if (d2 < best) {
                best = d2;
                in.aim = std::atan2(dy, dx);
                fire = true;
            }
        });
    return in;
}

}  // namespace arena
