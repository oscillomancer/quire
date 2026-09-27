#include "bridge_protocol.h"

#include <poll.h>
#include <sys/socket.h>

#include <cerrno>

namespace supple::host::bridge {

namespace {

bool writeAll(int s, const void* data, std::size_t n, int fd) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    bool fdPending = fd >= 0;
    while (n > 0) {
        iovec iov{const_cast<std::uint8_t*>(p), n};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
        if (fdPending) {
            std::memset(control, 0, sizeof control);
            msg.msg_control = control;
            msg.msg_controllen = sizeof control;
            cmsghdr* c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
        }
        const ssize_t w = ::sendmsg(s, &msg, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        fdPending = false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

bool readAll(int s, void* data, std::size_t n, int* fd, int timeoutMs) {
    auto* p = static_cast<std::uint8_t*>(data);
    while (n > 0) {
        if (timeoutMs >= 0) {
            pollfd pfd{s, POLLIN, 0};
            const int r = ::poll(&pfd, 1, timeoutMs);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;  // timeout or error
        }
        iovec iov{p, n};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
        if (fd) {
            msg.msg_control = control;
            msg.msg_controllen = sizeof control;
        }
        const ssize_t r = ::recvmsg(s, &msg, MSG_CMSG_CLOEXEC);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;  // the peer is gone
        if (fd) {
            for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                    std::memcpy(fd, CMSG_DATA(c), sizeof(int));
            fd = nullptr;  // only with the first bytes
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

}  // namespace

bool sendFrame(int socket, std::uint32_t head, std::uint32_t instance,
               const std::vector<std::uint8_t>& payload, int fd) {
    std::uint32_t prefix[3] = {static_cast<std::uint32_t>(payload.size()), head, instance};
    return writeAll(socket, prefix, sizeof prefix, fd) &&
           (payload.empty() || writeAll(socket, payload.data(), payload.size(), -1));
}

bool recvFrame(int socket, std::uint32_t& head, std::uint32_t& instance,
               std::vector<std::uint8_t>& payload, int* fd, int timeoutMs) {
    if (fd) *fd = -1;
    std::uint32_t prefix[3] = {0, 0, 0};
    if (!readAll(socket, prefix, sizeof prefix, fd, timeoutMs)) return false;
    if (prefix[0] > kMaxMessage) return false;
    head = prefix[1];
    instance = prefix[2];
    payload.resize(prefix[0]);
    return prefix[0] == 0 || readAll(socket, payload.data(), payload.size(), nullptr, timeoutMs);
}

}  // namespace supple::host::bridge
