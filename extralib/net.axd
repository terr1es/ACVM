//  net.axd — TCP, the part that has to call the operating system.
//
//  `library "net"` means these functions live in net.dll (Windows) / libnet.so.
//  A standard library module reaches them by importing the module:
//
//      function import (return:(net));    // resolves to net.axd + net.ax
//
//  ── importing this module is a promise ────────────────────────────────────
//
//  Nothing here has an Axiom fallback: opening a socket is not an expression the
//  language can evaluate. A program that imports `net` needs net.dll, and without
//  it the first call fails with `cannot load library 'net'`. Keep it out of
//  portable code.
//
//  ── three layers, one import ──────────────────────────────────────────────
//
//  stdlib/net.ax wraps these, and the wrapper is what most programs should call:
//
//    net.axd  (here)   handles, timeouts, errno. Every call can fail; the caller
//                      owns the handle and must close it.
//    net.ax            the same operations with the failures folded away:
//                      `fetchAll` opens, sends, reads to EOF, closes -- one call,
//                      impossible to leak.
//
//  Both layers are public. Reach for the raw form when you need a connection to
//  outlive one function (a server, a long-lived session); use the wrapper
//  otherwise.
//
//  ── handles are `int64`, and that is a correctness requirement ────────────
//
//  A socket is `int` on POSIX but `SOCKET` on Windows, which is `UINT_PTR` --
//  64 bits on x64. An `int` handle would truncate it and the truncation would
//  surface as "connect succeeded, then every call goes to a dead handle". So the
//  handle type is `int64` everywhere, and `-1` means "no handle".
//
//  ── no defer: you close it, and this is the trade ─────────────────────────
//
//  Axiom has no `defer` and no destructor, so a handle this module hands out stays
//  open until you call `tcpClose` or the process exits. Forgetting one leaks a
//  file descriptor for the lifetime of the process -- there is no finaliser to
//  save you, and no warning.
//
//  That is the price of exposing real sockets, and it is exactly why net.ax also
//  offers calls that open and close inside one function. Prefer those. When you do
//  hold a handle, keep the window small and use one shape:
//
//      int64 fd = tcpConnect("example.com", 80, 5000);
//      if (fd >= 0) {
//          ... use it ...
//          tcpClose(fd);          // every path out of the branch closes
//      }
//
//  A server holds two handles (listening + accepted) and closes each when its work
//  is done; a loop that `return`s early is where leaks come from.
//
//  ── error reporting ───────────────────────────────────────────────────────
//
//  Failure is -1 (or 0 for `tcpRecv`, meaning the peer closed). Nothing traps:
//  "connection refused" is information, and a library does not get to decide that
//  the caller's program should die.
//
//  The platform error code from the last failure on the **current thread** is
//  available from `netLastError` and printable with `netErrorText`. A code of 0
//  means "no code" -- currently only a timeout and a bad argument. `netTimedOut`
//  distinguishes the timeout case, which matters because -1 from
//  `tcpRecvTimeout` is otherwise ambiguous.
//
//  ── what is deliberately absent ───────────────────────────────────────────
//
//  UDP and raw sockets. UDP is a different contract -- no connection, so no
//  accept, and a datagram can be lost or duplicated, which changes every caller's
//  error handling. Raw sockets need elevated privileges. Neither is a small
//  addition to this file, and neither is needed to talk to an HTTP server or write
//  a TCP service. See extralib/encoding.ax for the list.
//
//  TLS is absent for a different reason: it needs a certificate store and a
//  handshake implementation, which is a dependency, not a syscall. Nothing here
//  pretends that TCP is secure -- pair it with a tunnel if you need that.

module "net" version "0.1.0";

//  It lets a package manager judge compatibility and answer "which version of this
//  module do I have". Name must match the file name (the compiler checks), version
//  must be MAJOR.MINOR.PATCH; 0.1.0 means unreleased -- API may change, not a placeholder.

library "net";

// ─── client ───────────────────────────────────────────────────────────────

//  Opens a TCP connection to `host:port`. Returns a handle, or -1.
//
//  `host` may be a name or a literal address; both go through the system
//  resolver, so `"127.0.0.1"`, `"::1"` and `"example.com"` are all valid.
//
//  `timeoutMs` < 0 waits until the OS gives up, which can be over a minute on an
//  unreachable host. A non-negative value bounds it. Pass a bound in anything
//  interactive: an unbounded connect to a blackholed address is indistinguishable
//  from a hang.
function tcpConnect(string host, int port, int timeoutMs):int64;

// ─── server ───────────────────────────────────────────────────────────────

//  Binds and listens on `port` (0 asks the OS for a free one, then `tcpLocalPort`
//  reports it). Returns a listening handle, or -1.
//
//  `SO_REUSEADDR` is set, so a restart does not fail with "address already in
//  use" while old connections sit in TIME_WAIT.
function tcpListen(int port, int backlog):int64;

//  Waits for one connection. Returns its handle, or -1.
//
//  `timeoutMs` < 0 waits forever. A bounded wait is what lets a server notice it
//  should stop: with an unbounded accept the only way out of the loop is for
//  somebody to connect. A timeout leaves the error code at 0.
function tcpAccept(int64 listenFd, int timeoutMs):int64;

//  The port this socket is bound to, or -1. How a listener created with port 0
//  discovers the port it actually got -- the only way to write a network test that
//  cannot fail because something else holds the port.
function tcpLocalPort(int64 fd):int64;

//  Writes the peer's numeric address (`a.b.c.d` or an IPv6 form) into `buffer`.
//  Returns the byte count, or -1 when it does not fit.
//
//  Numeric only, on purpose: a reverse lookup would make a logging call depend on
//  a resolver, and a log line is worth less than a stall.
function tcpPeerAddress(int64 fd, ptr unsigned int8 buffer, int capacity):int;

// ─── transfer ─────────────────────────────────────────────────────────────

//  Sends all of `data`. Returns the byte count, or -1.
//
//  There is no partial-send case to handle: this loops until every byte is
//  accepted, or reports failure. `send` may write fewer bytes than asked, and
//  treating that as success truncates the message under load while passing every
//  test on a quiet machine -- so the loop is here rather than in every caller.
function tcpSend(int64 fd, string data):int;

//  Receives up to `capacity` bytes into `buffer`, one byte per element.
//
//  The buffer is `ptr unsigned int8`, **not** `ptr char`. A `char` element is two
//  bytes wide, so `buffer + total` would advance at twice the byte rate and leave
//  a hole between chunks; a one-byte element keeps the arithmetic linear and
//  halves the memory a caller needs. `io` uses `ptr char` because it fills a
//  buffer once; a receiver appends.
//
//  Returns the byte count, **0 when the peer closed cleanly**, or -1 on error.
//  A read loop that stops on "not positive" is correct; one that stops only on -1
//  hangs when the peer is done.
function tcpRecv(int64 fd, ptr unsigned int8 buffer, int capacity):int;

//  `tcpRecv` bounded by `timeoutMs`. Returns the byte count, 0 for a clean close,
//  or -1 for either an error or a timeout -- `netTimedOut` tells those apart.
function tcpRecvTimeout(int64 fd, ptr unsigned int8 buffer, int capacity, int timeoutMs):int;

// ─── teardown ─────────────────────────────────────────────────────────────

//  Half-closes: sends FIN so the peer sees end of stream, while this end can
//  still read. This is how a request/response client says "that is all I am
//  sending" for protocols that carry no length prefix.
function tcpShutdownWrite(int64 fd):int;

//  Closes the handle. Returns 0, or -1.
//
//  Calling it twice is a bug, not a no-op: the handle number may already belong to
//  another socket by the time the second call lands.
function tcpClose(int64 fd):int;

// ─── diagnostics ──────────────────────────────────────────────────────────

//  The platform error code from the last failed call on this thread. 0 means the
//  failure carried no code (a timeout, or a rejected argument).
function netLastError():int64;

//  True when the last failure was a timeout rather than a socket error.
function netTimedOut():bool;

//  Writes a readable form of `code` into `buffer`. Returns the byte count, or -1
//  when the buffer is too small. Code 0 describes a timeout.
function netErrorText(int code, ptr unsigned int8 buffer, int capacity):int;

//  True when this module is usable. Always true when the library loaded; exists so
//  net.ax can report a missing net.dll as a message instead of a load failure.
function netAvailable():bool;
