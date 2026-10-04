#include "bridge_protocol.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <limits>

namespace supple::host::bridge {

namespace {

bool ready(int socket, short events, Deadline deadline) {
    for (;;) {
        int timeout = -1;
        if (deadline != Deadline::max()) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= Deadline::duration::zero()) return false;
            const auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
            timeout = static_cast<int>(std::min<std::int64_t>(ms, INT32_MAX));
        }
        pollfd pfd{socket, events, 0};
        const int result = ::poll(&pfd, 1, timeout);
        if (result < 0 && errno == EINTR) continue;
        return result > 0 && (pfd.revents & events);
    }
}
bool writeAll(int s, const void* data, std::size_t n, int fd, Deadline deadline) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    bool fdPending = fd >= 0;
    while (n > 0) {
        if (!ready(s, POLLOUT, deadline)) return false;
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
        const ssize_t w = ::sendmsg(s, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        if (w == 0) return false;
        fdPending = false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

bool readAll(int s, void* data, std::size_t n, int* fd, Deadline deadline) {
    auto* p = static_cast<std::uint8_t*>(data);
    while (n > 0) {
        if (!ready(s, POLLIN, deadline)) return false;
        iovec iov{p, n};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
        if (fd) {
            msg.msg_control = control;
            msg.msg_controllen = sizeof control;
        }
        const ssize_t r = ::recvmsg(s, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        if (r == 0) return false;  // the peer is gone
        if (fd) {
            for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
                const auto count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (std::size_t i = 0; i < count; ++i) {
                    int received = -1;
                    std::memcpy(&received, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                    if (*fd < 0)
                        *fd = received;
                    else
                        ::close(received);
                }
            }
            fd = nullptr;  // only with the first bytes
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

}  // namespace

bool sendFrame(int socket, std::uint32_t head, std::uint32_t instance,
               const std::vector<std::uint8_t>& payload, int fd, Deadline deadline) {
    if (payload.size() > kMaxMessage) return false;
    std::uint32_t prefix[3] = {static_cast<std::uint32_t>(payload.size()), head, instance};
    return writeAll(socket, prefix, sizeof prefix, fd, deadline) &&
           (payload.empty() || writeAll(socket, payload.data(), payload.size(), -1, deadline));
}

bool recvFrame(int socket, std::uint32_t& head, std::uint32_t& instance,
               std::vector<std::uint8_t>& payload, int* fd, int timeoutMs) {
    return recvFrameUntil(socket, head, instance, payload, fd, deadlineAfter(timeoutMs));
}

bool recvFrameUntil(int socket, std::uint32_t& head, std::uint32_t& instance,
                    std::vector<std::uint8_t>& payload, int* fd, Deadline deadline) {
    if (fd) *fd = -1;
    auto fail = [&] {
        if (fd && *fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
        return false;
    };
    std::uint32_t prefix[3] = {0, 0, 0};
    if (!readAll(socket, prefix, sizeof prefix, fd, deadline)) return fail();
    if (prefix[0] > kMaxMessage) return fail();
    head = prefix[1];
    instance = prefix[2];
    payload.resize(prefix[0]);
    if (prefix[0] && !readAll(socket, payload.data(), payload.size(), nullptr, deadline))
        return fail();
    return true;
}

bool validRequest(Op op, Reader r) {
    switch (op) {
        case Op::Create:
            for (int i = 0; i < 7; ++i) r.skipString();
            break;
        case Op::Activate:
            r.skip(20);
            break;
        case Op::ParamValue:
            r.skip(4);
            break;
        case Op::ParamText:
        case Op::SetParam:
        case Op::NormalizedParam:
        case Op::PlainParam:
            r.skip(12);
            break;
        case Op::ParamFromText:
            r.skip(4);
            r.skipString();
            break;
        case Op::GetPatch:
        case Op::Describe:
            r.skip(1);
            break;
        case Op::SetPatch:
            r.skipString();
            r.skip(4);
            break;
        case Op::LoadState:
            r.skipString();
            break;
        case Op::EditorOpen:
            r.skip(16);
            break;
        case Op::EditorResize:
            r.skip(8);
            break;
        case Op::Deactivate:
        case Op::Params:
        case Op::Latency:
        case Op::SemanticSchema:
        case Op::SaveState:
        case Op::Service:
        case Op::Destroy:
        case Op::EditorClose:
        case Op::EditorRequests:
            break;
        default:
            return false;
    }
    return r.done();
}

bool validReply(Op op, Reader r) {
    switch (op) {
        case Op::Create: {
            if (!r.u32()) return false;
            r.skip(15);
            break;
        }
        case Op::Params: {
            const auto count = r.u32();
            for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
                r.skip(4);
                r.skipString();
                r.skipString();
                r.skip(28);
            }
            break;
        }
        case Op::ParamValue:
        case Op::ParamFromText:
        case Op::NormalizedParam:
        case Op::PlainParam:
            r.skip(8);
            break;
        case Op::ParamText:
        case Op::SemanticSchema:
        case Op::GetPatch:
        case Op::Describe:
        case Op::SaveState:
            r.skipString();
            break;
        case Op::Latency:
            r.skip(9);
            break;
        case Op::Service: {
            r.skip(15);
            const auto lines = r.u32();
            for (std::uint32_t i = 0; i < lines && r.ok(); ++i) r.skipString();
            r.skip(9);
            if (r.u8()) (void)r.error();
            break;
        }
        case Op::EditorOpen:
        case Op::EditorResize:
            r.skip(9);
            break;
        case Op::EditorRequests:
            r.skip(12);
            break;
        case Op::Activate:
        case Op::Deactivate:
        case Op::SetParam:
        case Op::SetPatch:
        case Op::LoadState:
        case Op::Destroy:
        case Op::EditorClose:
            break;
        default:
            return false;
    }
    return r.done();
}

}  // namespace supple::host::bridge
