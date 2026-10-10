#pragma once
// mce_net, the optional networking module (ADR-0035). Link mce::mce_net to use it; a game that
// does not carries no socket code (mce_engine never contains it).
//
//   host.h         Host: a UDP endpoint (server or client) with reliable / sequenced / unreliable
//                  channels over ENet
//   bytes.h        ByteWriter / ByteReader: the little-endian message codec
//   replication.h  NetId, ReplicationServer / ReplicationClient: server-authoritative snapshots

#include "mce/net/bytes.h"
#include "mce/net/host.h"
#include "mce/net/replication.h"
