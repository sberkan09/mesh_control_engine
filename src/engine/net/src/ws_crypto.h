#pragma once
// Base64 for the WebSocket handshake (ws_crypto.cpp; SHA-1 is behind websocket_accept_key).

#include "mce/fixed_string.h"

namespace mce::net::detail {

// Standard base64 with padding, of at most 45 bytes.
FixedString<64> base64(const u8* data, usize size);

}  // namespace mce::net::detail
