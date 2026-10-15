// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using TestSocket = SOCKET;
using TestSocketLength = int;
inline constexpr TestSocket kInvalidSocket = INVALID_SOCKET;
inline void closeTestSocket(TestSocket s) { closesocket(s); }
inline void shutdownTestSocket(TestSocket s) { shutdown(s, SD_BOTH); }
inline void startTestSockets() {
    static const bool started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    REQUIRE(started);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using TestSocket = int;
using TestSocketLength = socklen_t;
inline constexpr TestSocket kInvalidSocket = -1;
inline void closeTestSocket(TestSocket s) { close(s); }
inline void shutdownTestSocket(TestSocket s) { shutdown(s, SHUT_RDWR); }
inline void startTestSockets() {}
#endif
inline void suppressTestSigpipe(TestSocket s) {
#if defined(__APPLE__)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)s;
#endif
}
inline bool sendTestBytes(TestSocket socket, std::string_view bytes) {
    while (!bytes.empty()) {
#if defined(__linux__)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto n = send(socket, bytes.data(), static_cast<int>(bytes.size()), flags);
        if (n <= 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}
