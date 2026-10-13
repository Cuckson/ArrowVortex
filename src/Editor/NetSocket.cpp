#include <Editor/NetSocket.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace Vortex {

// ================================================================================================
// Platform helpers.

#ifdef _WIN32
typedef SOCKET NativeSocket;
typedef int SockLen;
static const uintptr_t INVALID_HANDLE = static_cast<uintptr_t>(INVALID_SOCKET);
static int LastError() { return WSAGetLastError(); }
static bool IsWouldBlock(int err) {
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}
static void CloseNative(NativeSocket s) { closesocket(s); }
#else
typedef int NativeSocket;
typedef socklen_t SockLen;
static const uintptr_t INVALID_HANDLE = ~static_cast<uintptr_t>(0);
static int LastError() { return errno; }
static bool IsWouldBlock(int err) {
    return err == EWOULDBLOCK || err == EAGAIN || err == EINPROGRESS;
}
static void CloseNative(NativeSocket s) { ::close(s); }
#endif

static NativeSocket ToNative(uintptr_t handle) {
    return static_cast<NativeSocket>(handle);
}

static uintptr_t FromNative(NativeSocket s) {
#ifdef _WIN32
    return static_cast<uintptr_t>(s);
#else
    return s < 0 ? INVALID_HANDLE : static_cast<uintptr_t>(s);
#endif
}

static std::string ErrorText(const char* what) {
    return std::string(what) + " (error " + std::to_string(LastError()) + ")";
}

// ================================================================================================
// Library setup.

bool NetSocket::startup() {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void NetSocket::shutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

std::vector<std::string> NetSocket::localAddresses() {
    std::vector<std::string> out;
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0) return out;

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &list) != 0) return out;

    for (addrinfo* it = list; it; it = it->ai_next) {
        char buf[INET_ADDRSTRLEN] = {};
        auto* addr = reinterpret_cast<sockaddr_in*>(it->ai_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf))) {
            std::string ip = buf;
            if (ip.rfind("127.", 0) != 0 &&
                std::find(out.begin(), out.end(), ip) == out.end()) {
                out.push_back(ip);
            }
        }
    }
    freeaddrinfo(list);
    return out;
}

// ================================================================================================
// NetSocket.

NetSocket::~NetSocket() { close(); }

NetSocket::NetSocket() : myHandle(INVALID_HANDLE) {}

NetSocket::NetSocket(uintptr_t handle) : myHandle(handle) {}

NetSocket::NetSocket(NetSocket&& other) noexcept : myHandle(other.myHandle) {
    other.myHandle = INVALID_HANDLE;
}

NetSocket& NetSocket::operator=(NetSocket&& other) noexcept {
    if (this != &other) {
        close();
        myHandle = other.myHandle;
        other.myHandle = INVALID_HANDLE;
    }
    return *this;
}

bool NetSocket::isValid() const { return myHandle != INVALID_HANDLE; }

void NetSocket::close() {
    if (isValid()) {
        CloseNative(ToNative(myHandle));
        myHandle = INVALID_HANDLE;
    }
}

void NetSocket::mySetNonBlocking() {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(ToNative(myHandle), FIONBIO, &mode);
#else
    int flags = fcntl(ToNative(myHandle), F_GETFL, 0);
    fcntl(ToNative(myHandle), F_SETFL, flags | O_NONBLOCK);
#endif
    // Note edits are tiny; send them immediately.
    int noDelay = 1;
    setsockopt(ToNative(myHandle), IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
}

bool NetSocket::listen(int port, std::string& error) {
    close();
    myHandle = FromNative(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!isValid()) {
        error = ErrorText("Could not create socket");
        return false;
    }

    int reuse = 1;
    setsockopt(ToNative(myHandle), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(ToNative(myHandle), reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) != 0) {
        error =
            ErrorText(("Could not bind port " + std::to_string(port)).c_str());
        close();
        return false;
    }
    if (::listen(ToNative(myHandle), 1) != 0) {
        error = ErrorText("Could not listen");
        close();
        return false;
    }
    mySetNonBlocking();
    return true;
}

bool NetSocket::beginConnect(const std::string& host, int port,
                             std::string& error) {
    close();

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &list) != 0 ||
        !list) {
        error = "Could not resolve address \"" + host + "\"";
        return false;
    }

    myHandle = FromNative(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!isValid()) {
        freeaddrinfo(list);
        error = ErrorText("Could not create socket");
        return false;
    }
    mySetNonBlocking();

    int res = ::connect(ToNative(myHandle), list->ai_addr,
                        static_cast<SockLen>(list->ai_addrlen));
    freeaddrinfo(list);
    if (res != 0 && !IsWouldBlock(LastError())) {
        error = ErrorText("Could not connect");
        close();
        return false;
    }
    return true;
}

bool NetSocket::finishConnect(std::string& error) {
    int err = 0;
    SockLen len = sizeof(err);
    getsockopt(ToNative(myHandle), SOL_SOCKET, SO_ERROR,
               reinterpret_cast<char*>(&err), &len);
    if (err != 0) {
        error = "Could not connect (error " + std::to_string(err) + ")";
        return false;
    }
    return true;
}

NetSocket NetSocket::accept() {
    NativeSocket s = ::accept(ToNative(myHandle), nullptr, nullptr);
    NetSocket out(FromNative(s));
    if (out.isValid()) out.mySetNonBlocking();
    return out;
}

int NetSocket::poll(bool wantWrite, int timeoutMs) const {
    if (!isValid()) return POLL_ERROR;

    fd_set readSet, writeSet, errorSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    NativeSocket s = ToNative(myHandle);
    FD_SET(s, &readSet);
    FD_SET(s, &errorSet);
    if (wantWrite) FD_SET(s, &writeSet);

    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

#ifdef _WIN32
    int res = select(0, &readSet, &writeSet, &errorSet, &tv);
#else
    int res = select(s + 1, &readSet, &writeSet, &errorSet, &tv);
#endif
    if (res < 0) return POLL_ERROR;

    int flags = 0;
    if (FD_ISSET(s, &readSet)) flags |= POLL_READ;
    if (FD_ISSET(s, &writeSet)) flags |= POLL_WRITE;
    if (FD_ISSET(s, &errorSet)) flags |= POLL_ERROR;
    return flags;
}

int NetSocket::send(const void* data, int size) {
#ifdef _WIN32
    int res =
        ::send(ToNative(myHandle), static_cast<const char*>(data), size, 0);
#else
    int res =
        static_cast<int>(::send(ToNative(myHandle), data, size, MSG_NOSIGNAL));
#endif
    if (res < 0) return IsWouldBlock(LastError()) ? 0 : -1;
    return res;
}

int NetSocket::recv(void* data, int size) {
#ifdef _WIN32
    int res = ::recv(ToNative(myHandle), static_cast<char*>(data), size, 0);
#else
    int res = static_cast<int>(::recv(ToNative(myHandle), data, size, 0));
#endif
    if (res == 0) return -1;  // Orderly shutdown by the peer.
    if (res < 0) return IsWouldBlock(LastError()) ? 0 : -1;
    return res;
}

};  // namespace Vortex
