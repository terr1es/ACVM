//  net.c — TCP sockets.
//
//  Built into net.dll / libnet.so and reached from Axiom through stdlib/net.axd.
//
//  ── what is here, and what is not ─────────────────────────────────────────
//
//  This is the *transport*: connect, listen, accept, send, receive, close. That is
//  the part that cannot be written in Axiom, because it is a set of system calls.
//
//  Everything above the transport -- connection strings, reading a whole response
//  until the peer closes, retry loops, formatting an address -- is ordinary code
//  and lives in stdlib/net.ax. Keeping the boundary at the syscall means a
//  program that wants an HTTP GET needs no new C, and the C here stays small
//  enough to audit.
//
//  UDP and raw sockets are absent. UDP is a different contract (no connection, so
//  no `accept` and different failure semantics) and raw sockets need privileges;
//  neither is a small addition to this file. See extralib/encoding.ax for the list and
//  the reason.
//
//  ── handles are int64, and that is not cosmetic ───────────────────────────
//
//  A socket handle is `int` on POSIX and `SOCKET` on Windows -- and `SOCKET` is
//  `UINT_PTR`, which is 64 bits on x64. An `int` handle type would truncate it
//  silently and the failure would look like "connect succeeded, send goes to a
//  dead handle". So the public handle type is `int64` and this file casts.
//
//  `-1` means "no handle". That is also `INVALID_SOCKET` on Windows and the
//  convention for a failed fd on POSIX, so one comparison covers both.
//
//  ── no defer: the caller closes, and this is the trade ────────────────────
//
//  Axiom has no `defer` and no destructor, so nothing here can close a socket on
//  the caller's behalf. A leaked handle holds a file descriptor until the process
//  exits. This is the price of exposing raw handles, and it is why stdlib/net.ax
//  also offers calls that open and close inside one function: those cannot leak.
//  Use them unless you need a long-lived connection or a server.
//
//  ── error reporting ───────────────────────────────────────────────────────
//
//  Every call returns -1 (or, for `recv`, a negative value) and stores the
//  platform error code, retrievable with `netLastError` and printable with
//  `netErrorText`. Nothing traps: a library cannot decide that the caller's
//  program should die, and "connection refused" is information, not a crash.
//
//  `tcpRecv` returns **0 for a clean close by the peer** and a negative value for
//  a failure, because those are different answers and a caller that conflates
//  them will hang or truncate.
//
//  The error slot is thread-local. `parallel` calls into C from several threads,
//  and one thread's "connection reset" must not overwrite another's.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
// winsock2.h must come before windows.h, or windows.h drags in the 1.1 headers
// and the build fails on redefinition. This is the single most common way to
// break a Winsock translation unit.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET acvm_socket;
typedef int acvm_socklen;
#define ACVM_INVALID_SOCKET INVALID_SOCKET
#define ACVM_SOCK_ERRNO WSAGetLastError()
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int acvm_socket;
typedef socklen_t acvm_socklen;
#define ACVM_INVALID_SOCKET (-1)
#define ACVM_SOCK_ERRNO errno
#endif

//===--- plumbing ---------------------------------------------------------===//

#if defined(_MSC_VER)
#define ACVM_THREAD_LOCAL __declspec(thread)
#else
#define ACVM_THREAD_LOCAL _Thread_local
#endif

/// The last failure on this thread: a platform error code, 0 for a timeout, or
/// -1 for "nothing has failed".
///
/// -1 rather than 0 for "no error", because 0 is the code a timeout carries and
/// the two are different answers. Platform socket errors are always positive, so
/// -1 cannot collide with a real one. A fresh thread therefore reports "no error"
/// instead of claiming the first query timed out.
static ACVM_THREAD_LOCAL int64_t gLastError = -1;

static int64_t fail(void) {
    gLastError = (int64_t)ACVM_SOCK_ERRNO;
    return -1;
}

/// One-time Winsock startup.
///
/// Deliberately not guarded by a lock. The VM calls into C from one thread, and
/// the only other caller is `parallel`; if two threads race here, both calls
/// succeed and the reference count is merely one higher than needed. WSAStartup
/// is idempotent in exactly that sense, so a lock would buy nothing and add a
/// failure path. `WSACleanup` is never called: the process is exiting when it
/// would matter, and the OS reclaims the sockets then.
static int ensure_net(void) {
#ifdef _WIN32
    static volatile long started = 0;
    if (started != 0) {
        return 0;
    }
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        gLastError = (int64_t)ACVM_SOCK_ERRNO;
        return -1;
    }
    started = 1;
#endif
    return 0;
}

static acvm_socket to_socket(int64_t handle) {
    return (acvm_socket)handle;
}

static int64_t from_socket(acvm_socket sock) {
    return (int64_t)sock;
}

/// True when `err` means "this would have blocked" rather than "this failed".
///
/// `WSAEINVAL` is deliberately not in the list. It means "invalid argument" to
/// every Winsock call, and `tcpSend` treats "would block" as "retry" -- so a
/// genuine bad-argument error would spin forever instead of being reported. The
/// non-blocking connect path reports `WSAEWOULDBLOCK`, which is enough.
static int is_would_block(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EWOULDBLOCK || err == EINPROGRESS || err == EAGAIN;
#endif
}

/// Puts `socket` in non-blocking or blocking mode. Used by the connect and accept
/// timeouts, both of which are built on `select` rather than on socket options:
/// `SO_RCVTIMEO` exists on both platforms but reports a timeout as "would block"
/// on Windows and as `EAGAIN` on Linux, and a timeout is a different answer from
/// "no data yet".
static int set_nonblocking(acvm_socket sock, int nonblocking) {
#ifdef _WIN32
    u_long mode = nonblocking ? 1UL : 0UL;
    return ioctlsocket(sock, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    const int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    const int updated = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(sock, F_SETFL, updated) == 0 ? 0 : -1;
#endif
}

/// Waits until `socket` is readable, writable, or both, or `timeoutMs` passes.
/// Returns 1 when ready, 0 on timeout, -1 on error. A negative timeout waits
/// forever.
static int wait_ready(acvm_socket sock, int wantWrite, int64_t timeoutMs) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(sock, &set);

    struct timeval tv;
    struct timeval* tvp = NULL;
    if (timeoutMs >= 0) {
        tv.tv_sec = (long)(timeoutMs / 1000);
        tv.tv_usec = (long)((timeoutMs % 1000) * 1000);
        tvp = &tv;
    }

    // The nfds argument is ignored on Windows and means "highest fd + 1" on
    // POSIX; the +1 is harmless for a handle that is not a small integer.
    const int ready = select((int)(sock + 1), wantWrite ? NULL : &set, wantWrite ? &set : NULL,
                             NULL, tvp);
    if (ready < 0) {
        return is_would_block(ACVM_SOCK_ERRNO) ? 0 : -1;
    }
    return ready > 0 ? 1 : 0;
}

/// Waits for a non-blocking connect to finish. Returns 1 when it finished
/// (successfully or not -- check SO_ERROR), 0 on timeout, -1 on error.
///
/// `writefds` alone is not enough on Windows: MSDN puts a *failed* connect in
/// `exceptfds`, while a successful one lands in `writefds`. A version that watched
/// only for writability would sit until the timeout and then report "timed out"
/// for what was really "refused" -- the wrong diagnosis, and a slow one. Watching
/// both means either outcome wakes it up, and SO_ERROR says which it was.
static int wait_connected(acvm_socket sock, int64_t timeoutMs) {
    fd_set writeSet;
    fd_set exceptSet;
    FD_ZERO(&writeSet);
    FD_SET(sock, &writeSet);
    FD_ZERO(&exceptSet);
    FD_SET(sock, &exceptSet);

    struct timeval tv;
    struct timeval* tvp = NULL;
    if (timeoutMs >= 0) {
        tv.tv_sec = (long)(timeoutMs / 1000);
        tv.tv_usec = (long)((timeoutMs % 1000) * 1000);
        tvp = &tv;
    }
    const int ready =
        select((int)(sock + 1), NULL, &writeSet, &exceptSet, tvp);
    if (ready < 0) {
        return is_would_block(ACVM_SOCK_ERRNO) ? 0 : -1;
    }
    return ready > 0 ? 1 : 0;
}

/// TCP_NODELAY on the accepted/connected socket.
///
/// Nagle's algorithm batches small writes, which is right for bulk transfer and
/// wrong for anything request/response shaped: a 40 byte request waits for an ACK
/// that will never come sooner because the peer is waiting for the request. Every
/// protocol above this layer is request/response, so the delay is the common case
/// and the batching gain is the rare one.
static void disable_nagle(acvm_socket sock) {
    int on = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
}

/// Closes without touching the error slot, for cleanup paths where the original
/// error is the one worth reporting.
static void close_quietly(acvm_socket sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

/// Fills a sockaddr_storage with `host`/`port` and opens a socket for it.
///
/// `getaddrinfo` is used even when `host` is a literal address: it accepts both,
/// so a caller never has to decide which kind of string it has. The first
/// resolved address wins -- trying every candidate in turn would hide a real
/// failure behind a slow one, and the caller sees the error anyway.
static int64_t resolve_and_open(const char* host, int64_t hostLength, int64_t port,
                                struct sockaddr_storage* out, acvm_socklen* outLength) {
    char name[256];
    if (host == NULL || hostLength <= 0 || hostLength >= (int64_t)sizeof(name)) {
        gLastError = -1;
        return -1;
    }
    memcpy(name, host, (size_t)hostLength);
    name[hostLength] = 0;

    char service[16];
    snprintf(service, sizeof(service), "%ld", (long)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;      // IPv4 or IPv6, whichever the name has
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* resolved = NULL;
    const int rc = getaddrinfo(name, service, &hints, &resolved);
    if (rc != 0) {
        // getaddrinfo reports through its own return value, not errno, so the
        // platform error slot would still hold the previous call's number.
        gLastError = (int64_t)rc;
        return -1;
    }

    const acvm_socket sock =
        socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
    if (sock == ACVM_INVALID_SOCKET) {
        freeaddrinfo(resolved);
        return fail();
    }
    if (resolved->ai_addrlen > sizeof(*out)) {
        freeaddrinfo(resolved);
        close_quietly(sock);
        gLastError = -1;
        return -1;
    }
    memcpy(out, resolved->ai_addr, resolved->ai_addrlen);
    *outLength = (acvm_socklen)resolved->ai_addrlen;
    freeaddrinfo(resolved);
    return from_socket(sock);
}

//===--- client -----------------------------------------------------------===//

/// Opens a TCP connection to `host:port`. Returns a handle, or -1.
///
/// `timeoutMs` < 0 waits until the OS gives up (which can be over a minute on an
/// unreachable host). A non-negative value bounds the wait: the socket is put in
/// non-blocking mode only for the connect, then restored, because a non-blocking
/// socket would make every later `send` return "would block" and force the caller
/// into a polling loop it never asked for.
int64_t tcpConnect(const char* host, int64_t hostLength, int64_t port, int64_t timeoutMs) {
    if (ensure_net() != 0) {
        return -1;
    }
    if (port <= 0 || port > 65535) {
        gLastError = -1;
        return -1;
    }

    struct sockaddr_storage address;
    acvm_socklen addressLength = 0;
    const int64_t opened = resolve_and_open(host, hostLength, port, &address, &addressLength);
    if (opened < 0) {
        return -1;
    }
    acvm_socket socket = to_socket(opened);

    if (timeoutMs >= 0) {
        if (set_nonblocking(socket, 1) != 0) {
            close_quietly(socket);
            return fail();
        }
        const int rc = connect(socket, (struct sockaddr*)&address, addressLength);
        if (rc != 0) {
            const int err = ACVM_SOCK_ERRNO;
            if (!is_would_block(err)) {
                close_quietly(socket);
                gLastError = (int64_t)err;
                return -1;
            }
            const int ready = wait_connected(socket, timeoutMs);
            if (ready < 0) {
                const int waitErr = ACVM_SOCK_ERRNO;
                close_quietly(socket);
                gLastError = (int64_t)waitErr;
                return -1;
            }
            if (ready == 0) {
                // A timeout carries code 0 -- distinct from -1, which means
                // nothing failed, and from a positive platform error.
                close_quietly(socket);
                gLastError = 0;
                return -1;
            }
            // select said writable, which for a connecting socket means the
            // attempt finished -- successfully or not. SO_ERROR is the answer.
            int pending = 0;
            acvm_socklen pendingLength = (acvm_socklen)sizeof(pending);
            if (getsockopt(socket, SOL_SOCKET, SO_ERROR, (char*)&pending, &pendingLength) != 0) {
                const int sockErr = ACVM_SOCK_ERRNO;
                close_quietly(socket);
                gLastError = (int64_t)sockErr;
                return -1;
            }
            if (pending != 0) {
                close_quietly(socket);
                gLastError = (int64_t)pending;
                return -1;
            }
        }
        if (set_nonblocking(socket, 0) != 0) {
            close_quietly(socket);
            return fail();
        }
    } else {
        if (connect(socket, (struct sockaddr*)&address, addressLength) != 0) {
            // The code has to be read *before* the close: closesocket() sets its
            // own error, so calling fail() afterwards reports the close rather
            // than the connect.
            const int connectErr = ACVM_SOCK_ERRNO;
            close_quietly(socket);
            gLastError = (int64_t)connectErr;
            return -1;
        }
    }

    disable_nagle(socket);
    return from_socket(socket);
}

//===--- server -----------------------------------------------------------===//

/// Binds and listens. Returns a listening handle, or -1.
///
/// `port` 0 asks the OS for a free port, which `tcpLocalPort` then reports. That
/// combination is the only way to write a test that cannot fail because something
/// else holds the port, so it is supported on purpose rather than left to chance.
///
/// `SO_REUSEADDR` is set. Without it a server that restarts cannot bind while the
/// old connection sits in TIME_WAIT -- on POSIX that is a ~60 second outage after
/// every restart, and the error (`address already in use`) points at the wrong
/// problem.
int64_t tcpListen(int64_t port, int64_t backlog) {
    if (ensure_net() != 0) {
        return -1;
    }
    if (port < 0 || port > 65535) {
        gLastError = -1;
        return -1;
    }
    if (backlog <= 0) {
        backlog = 16;
    }
    if (backlog > 4096) {
        backlog = 4096;
    }

    // IPv4 deliberately: a dual-stack IPv6 listener behaves differently on every
    // platform (v6only defaults differ), and "it worked on Linux" is not a good
    // reason for a surprise on Windows. IPv6 literals still work through
    // `tcpConnect`, which resolves.
    const acvm_socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == ACVM_INVALID_SOCKET) {
        return fail();
    }

    int on = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)port);

    if (bind(listener, (struct sockaddr*)&address, (acvm_socklen)sizeof(address)) != 0) {
        const int err = ACVM_SOCK_ERRNO;
        close_quietly(listener);
        gLastError = (int64_t)err;
        return -1;
    }
    if (listen(listener, (int)backlog) != 0) {
        const int err = ACVM_SOCK_ERRNO;
        close_quietly(listener);
        gLastError = (int64_t)err;
        return -1;
    }
    return from_socket(listener);
}

/// Waits for one incoming connection. Returns its handle, or -1.
///
/// `timeoutMs` < 0 waits forever. A bounded wait is what lets a server notice
/// that it should stop: with an unbounded accept, the only way out of the loop is
/// for a client to connect. A timeout reports 0 in the error slot, since "nothing
/// arrived yet" is not an error.
int64_t tcpAccept(int64_t listenFd, int64_t timeoutMs) {
    if (ensure_net() != 0) {
        return -1;
    }
    acvm_socket listener = to_socket(listenFd);
    if (listener == ACVM_INVALID_SOCKET) {
        gLastError = -1;
        return -1;
    }

    if (timeoutMs >= 0) {
        const int ready = wait_ready(listener, /*wantWrite=*/0, timeoutMs);
        if (ready < 0) {
            return fail();
        }
        if (ready == 0) {
            gLastError = 0;
            return -1;
        }
    }

    struct sockaddr_storage peer;
    acvm_socklen peerLength = (acvm_socklen)sizeof(peer);
    const acvm_socket accepted = accept(listener, (struct sockaddr*)&peer, &peerLength);
    if (accepted == ACVM_INVALID_SOCKET) {
        return fail();
    }
    disable_nagle(accepted);
    return from_socket(accepted);
}

/// The port this socket is bound to, or -1. For a listener created with port 0
/// this is how the actual port is discovered.
int64_t tcpLocalPort(int64_t fd) {
    acvm_socket socket = to_socket(fd);
    if (socket == ACVM_INVALID_SOCKET) {
        gLastError = -1;
        return -1;
    }
    struct sockaddr_storage address;
    acvm_socklen length = (acvm_socklen)sizeof(address);
    if (getsockname(socket, (struct sockaddr*)&address, &length) != 0) {
        return fail();
    }
    if (address.ss_family == AF_INET) {
        return (int64_t)ntohs(((struct sockaddr_in*)&address)->sin_port);
    }
    if (address.ss_family == AF_INET6) {
        return (int64_t)ntohs(((struct sockaddr_in6*)&address)->sin6_port);
    }
    gLastError = -1;
    return -1;
}

/// Writes the peer's address as `a.b.c.d` (or an IPv6 form) into `buffer`.
/// Returns the byte count, or -1 when it does not fit. Numeric only: a reverse
/// lookup would make a logging call depend on a resolver.
int64_t tcpPeerAddress(int64_t fd, uint8_t* buffer, int64_t capacity) {
    acvm_socket socket = to_socket(fd);
    if (socket == ACVM_INVALID_SOCKET || buffer == NULL || capacity <= 0) {
        gLastError = -1;
        return -1;
    }
    struct sockaddr_storage address;
    acvm_socklen length = (acvm_socklen)sizeof(address);
    if (getpeername(socket, (struct sockaddr*)&address, &length) != 0) {
        return fail();
    }

    char text[64];
    const void* raw = NULL;
    if (address.ss_family == AF_INET) {
        raw = &((struct sockaddr_in*)&address)->sin_addr;
    } else if (address.ss_family == AF_INET6) {
        raw = &((struct sockaddr_in6*)&address)->sin6_addr;
    } else {
        gLastError = -1;
        return -1;
    }
    if (inet_ntop(address.ss_family, raw, text, sizeof(text)) == NULL) {
        return fail();
    }

    const int64_t bytes = (int64_t)strlen(text);
    if (bytes > capacity) {
        gLastError = -1;
        return -1;   // too small is an error, not a truncation
    }
    memcpy(buffer, text, (size_t)bytes);
    return bytes;
}

//===--- transfer ---------------------------------------------------------===//

/// Sends `length` bytes, looping until all of them are accepted or an error
/// occurs. Returns `length`, or -1.
///
/// The loop is the whole point. `send` on a blocking socket may write fewer bytes
/// than asked -- it returns what the kernel buffered -- and code that treats that
/// as failure, or that ignores it, truncates the message under load and works
/// fine in every test. This is the single most common bug in hand-written socket
/// code, so it is not left to the caller.
int64_t tcpSend(int64_t fd, const char* data, int64_t length) {
    acvm_socket socket = to_socket(fd);
    if (socket == ACVM_INVALID_SOCKET) {
        gLastError = -1;
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    if (data == NULL || length < 0) {
        gLastError = -1;
        return -1;
    }

    int64_t sent = 0;
    while (sent < length) {
        const int chunk =
            send(socket, data + sent, (int)((length - sent) > 0x7FFFFFFF ? 0x7FFFFFFF
                                                                        : (length - sent)),
                 0);
        if (chunk <= 0) {
            if (chunk < 0 && is_would_block(ACVM_SOCK_ERRNO)) {
                continue;   // blocking socket should not get here; be safe anyway
            }
            return fail();
        }
        sent += (int64_t)chunk;
    }
    return sent;
}

/// Receives up to `capacity` bytes into `buffer`. One byte per element.
///
/// `uint8_t*`, not the `uint16_t*` that `ptr char` maps to elsewhere: a receiver
/// appends chunk after chunk, and an element count of 2-byte units makes
/// `buffer + total` advance at twice the byte rate, leaving a hole between
/// chunks. A one-byte element makes the arithmetic linear, and halves the buffer
/// a caller has to allocate for the same number of bytes.
///
/// Returns the byte count, **0 when the peer closed cleanly**, or -1 on error.
/// The three-way result is deliberate: a loop that stops on "not positive" is
/// correct, while one that stops only on -1 hangs when the peer is done.
int64_t tcpRecv(int64_t fd, uint8_t* buffer, int64_t capacity) {
    acvm_socket socket = to_socket(fd);
    if (socket == ACVM_INVALID_SOCKET || buffer == NULL || capacity <= 0) {
        gLastError = -1;
        return -1;
    }
    const int room = capacity > 0x7FFFFFFF ? 0x7FFFFFFF : (int)capacity;
    const int got = recv(socket, (char*)buffer, room, 0);
    if (got == 0) {
        return 0;   // orderly shutdown by the peer
    }
    if (got < 0) {
        return fail();
    }
    return (int64_t)got;
}

/// `tcpRecv` with a bound on how long to wait. Returns the byte count, 0 for a
/// clean close, or -1 for an error **or a timeout** -- `netTimedOut` tells the two
/// apart.
int64_t tcpRecvTimeout(int64_t fd, uint8_t* buffer, int64_t capacity, int64_t timeoutMs) {
    acvm_socket socket = to_socket(fd);
    if (socket == ACVM_INVALID_SOCKET || buffer == NULL || capacity <= 0) {
        gLastError = -1;
        return -1;
    }
    const int ready = wait_ready(socket, /*wantWrite=*/0, timeoutMs);
    if (ready < 0) {
        return fail();
    }
    if (ready == 0) {
        gLastError = 0;   // timed out; not a socket error
        return -1;
    }
    return tcpRecv(fd, buffer, capacity);
}

//===--- teardown ---------------------------------------------------------===//

/// Half-closes: sends FIN, so the peer sees end of stream, while this end can
/// still read. This is what a request/response client wants after sending its
/// request -- it is how the peer learns the request is complete, for protocols
/// that do not carry a length.
int64_t tcpShutdownWrite(int64_t fd) {
    acvm_socket socket = to_socket(fd);
    if (socket == ACVM_INVALID_SOCKET) {
        gLastError = -1;
        return -1;
    }
#ifdef _WIN32
    if (shutdown(socket, SD_SEND) != 0) {
        return fail();
    }
#else
    if (shutdown(socket, SHUT_WR) != 0) {
        return fail();
    }
#endif
    return 0;
}

/// Closes the handle. Returns 0 or -1. Safe to call twice only in the sense that
/// the second call reports -1: the handle number may have been reused by then.
int64_t tcpClose(int64_t fd) {
    acvm_socket socket = to_socket(fd);
    if (socket == ACVM_INVALID_SOCKET) {
        gLastError = -1;
        return -1;
    }
#ifdef _WIN32
    if (closesocket(socket) != 0) {
        return fail();
    }
#else
    if (close(socket) != 0) {
        return fail();
    }
#endif
    return 0;
}

//===--- diagnostics -----------------------------------------------------===//

/// The platform error code from the last failed call on this thread.
/// -1 means nothing has failed, 0 means the failure was a timeout, and a positive
/// number is the platform's own code.
int64_t netLastError(void) {
    return gLastError;
}

/// True when the last failure was a timeout rather than a socket error.
int64_t netTimedOut(void) {
    return gLastError == 0 ? 1 : 0;
}

/// Writes a human-readable form of `code` into `buffer`. Returns the byte count,
/// or -1 when the buffer is too small. `code` 0 describes a timeout.
int64_t netErrorText(int64_t code, uint8_t* buffer, int64_t capacity) {
    if (buffer == NULL || capacity <= 0) {
        return -1;
    }
    char text[256];
    if (code < 0) {
        return 0;   // nothing has failed; an empty description
    }
    if (code == 0) {
        snprintf(text, sizeof(text), "timed out");
    }
#ifdef _WIN32
    else {
        // FormatMessageA would work, but it appends a newline and its output is
        // localised; a table of the handful of errors a TCP client actually hits
        // is more predictable and reads the same everywhere.
        switch ((int)code) {
        case WSAECONNREFUSED: snprintf(text, sizeof(text), "connection refused"); break;
        case WSAETIMEDOUT: snprintf(text, sizeof(text), "connection timed out"); break;
        case WSAENETUNREACH: snprintf(text, sizeof(text), "network unreachable"); break;
        case WSAEHOSTUNREACH: snprintf(text, sizeof(text), "host unreachable"); break;
        case WSAECONNRESET: snprintf(text, sizeof(text), "connection reset by peer"); break;
        case WSAECONNABORTED: snprintf(text, sizeof(text), "connection aborted"); break;
        case WSAEADDRINUSE: snprintf(text, sizeof(text), "address already in use"); break;
        case WSAEACCES: snprintf(text, sizeof(text), "permission denied"); break;
        case WSAHOST_NOT_FOUND: snprintf(text, sizeof(text), "host not found"); break;
        case WSATRY_AGAIN: snprintf(text, sizeof(text), "temporary name resolution failure"); break;
        case WSAENOTCONN: snprintf(text, sizeof(text), "socket is not connected"); break;
        case WSAEWOULDBLOCK: snprintf(text, sizeof(text), "operation would block"); break;
        default: snprintf(text, sizeof(text), "socket error %d", (int)code); break;
        }
    }
#else
    else {
        switch ((int)code) {
        case ECONNREFUSED: snprintf(text, sizeof(text), "connection refused"); break;
        case ETIMEDOUT: snprintf(text, sizeof(text), "connection timed out"); break;
        case ENETUNREACH: snprintf(text, sizeof(text), "network unreachable"); break;
        case EHOSTUNREACH: snprintf(text, sizeof(text), "host unreachable"); break;
        case ECONNRESET: snprintf(text, sizeof(text), "connection reset by peer"); break;
        case EPIPE: snprintf(text, sizeof(text), "broken pipe"); break;
        case EADDRINUSE: snprintf(text, sizeof(text), "address already in use"); break;
        case EACCES: snprintf(text, sizeof(text), "permission denied"); break;
        case ENOTCONN: snprintf(text, sizeof(text), "socket is not connected"); break;
        case EAGAIN: snprintf(text, sizeof(text), "operation would block"); break;
        default: snprintf(text, sizeof(text), "socket error %d", (int)code); break;
        }
    }
#endif

    const int64_t bytes = (int64_t)strlen(text);
    if (bytes > capacity) {
        gLastError = -1;
        return -1;
    }
    memcpy(buffer, text, (size_t)bytes);
    return bytes;
}

/// True when the platform provides this module at all. Always 1 here; it exists so
/// stdlib/net.ax can report "net.dll is missing" as a message rather than as a
/// link error.
int64_t netAvailable(void) {
    return ensure_net() == 0 ? 1 : 0;
}
