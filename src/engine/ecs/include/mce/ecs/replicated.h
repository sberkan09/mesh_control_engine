#pragma once
// Replication as data (M-NET2 step 7): what a scene says about multiplayer, with no code. The
// components live in mce_ecs, which has no network code, so every scene, the editor, the cook and
// MCP know them; only a game that links mce_net acts on them (mce/net/replication.h):
//
//   NetReplicated   the server gives the entity a NetId (ReplicationServer::assign_tagged), so its
//                   LocalTransform and every component field flagged Replicated reach the clients
//   NetOwner        the peer that controls the entity, as the server numbers peers (through a relay
//                   room that is the member id, the same on every member); it is replicated itself
//   NetRelevance    with per-client snapshots, the entity reaches only clients that own an entity
//                   within `radius` of it (and the client that owns it); without it, or with
//                   radius 0, every client gets it
//
// A field opts in to replication with the Replicated attribute, the way Save opts in to saves:
//
//   struct Health { f32 hp = 100; f32 regen = 1; };
//   MCE_COMPONENT(Health, (hp, Replicated, Save), regen)   // clients see hp; regen stays on the server

#include "mce/refl/macros.h"

namespace mce {

struct NetReplicated {
    u32 kind = 0;
};
MCE_COMPONENT(NetReplicated,
              (kind, Help{"the game's archetype number; the client's on_spawn builds local-only parts from it"}))

struct NetOwner {
    u32 peer = 0xFFFFFFFFu;
};
MCE_COMPONENT(NetOwner, (peer, Replicated,
                         Help{"the peer that controls this entity, as the server numbers peers (4294967295 = nobody)"}))

struct NetRelevance {
    f32 radius = 0.0f;
};
MCE_COMPONENT(NetRelevance,
              (radius, Range{0.0f, 1000.0f},
               Help{"per-client snapshots send this entity only to clients with an owned entity this close (0 = all)"}))

}  // namespace mce
