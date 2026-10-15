#pragma once
// Minimal non-blocking TCP sockets for the WebSocket transport (M-NET2 step 6), private to mce_net.
// IPv4 only; POSIX and Winsock behind one interface. Every call is non-blocking.

#include "mce/types.h"

namespace mce::net::detail {

#if defined(_WIN32)
using SocketHandle = u64;  // SOCKET
#else
using SocketHandle = int;
#endif
SocketHandle invalid_socket();

// Process-wide setup (WSAStartup on Windows), counted.
bool sockets_acquire();
void sockets_release();

// Listens on address:port (port 0 picks one); returns the bound port through `bound_port`.
SocketHandle listen_tcp(const char* address, u16 port, u16& bound_port);
// A pending connection, or invalid_socket() when none.
SocketHandle accept_tcp(SocketHandle listener);
// Starts connecting (resolving `host`, which may block for a name); connect_state() says when done.
SocketHandle connect_tcp(const char* host, u16 port);
enum class ConnectState : u8 { Pending, Connected, Failed };
ConnectState connect_state(SocketHandle s);
void close_socket(SocketHandle s);
// No more sends (the other side reads EOF after what was sent).
void shutdown_send(SocketHandle s);

enum class IoResult : u8 { Ok, WouldBlock, Closed };
// Reads what is available (n set to the count, which may be 0 with WouldBlock).
IoResult recv_some(SocketHandle s, u8* data, usize capacity, usize& n);
// Writes what the socket takes (n set to the count).
IoResult send_some(SocketHandle s, const u8* data, usize size, usize& n);

}  // namespace mce::net::detail
