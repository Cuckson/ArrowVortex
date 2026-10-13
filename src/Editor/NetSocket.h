#pragma once

#include <Core/Core.h>

#include <string>
#include <vector>

namespace Vortex {

/// Minimal non-blocking IPv4 TCP socket (Winsock on Windows, BSD sockets
/// elsewhere). Not thread-safe; owned by a single thread at a time.
class NetSocket {
   public:
    enum PollFlags {
        POLL_READ = 1 << 0,
        POLL_WRITE = 1 << 1,
        POLL_ERROR = 1 << 2,
    };

    /// Initializes the socket library. Must be called before any other use.
    static bool startup();

    /// Releases the socket library.
    static void shutdown();

    /// Returns the IPv4 addresses of this machine (excluding loopback).
    static std::vector<std::string> localAddresses();

    ~NetSocket();
    NetSocket();
    NetSocket(NetSocket&& other) noexcept;
    NetSocket& operator=(NetSocket&& other) noexcept;
    NetSocket(const NetSocket&) = delete;
    NetSocket& operator=(const NetSocket&) = delete;

    /// Opens a listening socket on all interfaces at the given port.
    bool listen(int port, std::string& error);

    /// Starts a non-blocking connect. Use poll(POLL_WRITE) and
    /// finishConnect() to complete it.
    bool beginConnect(const std::string& host, int port, std::string& error);

    /// Returns true if a pending connect succeeded, false if it failed.
    bool finishConnect(std::string& error);

    /// Accepts a pending connection. Returns an invalid socket if none.
    NetSocket accept();

    /// Waits up to timeoutMs for the socket to become readable (and writable,
    /// if requested). Returns a combination of PollFlags.
    int poll(bool wantWrite, int timeoutMs) const;

    /// Returns the number of bytes sent, 0 if the call would block, or -1 if
    /// the connection is broken.
    int send(const void* data, int size);

    /// Returns the number of bytes received, 0 if the call would block, or -1
    /// if the connection is closed or broken.
    int recv(void* data, int size);

    bool isValid() const;
    void close();

   private:
    explicit NetSocket(uintptr_t handle);
    void mySetNonBlocking();

    uintptr_t myHandle;
};

};  // namespace Vortex
