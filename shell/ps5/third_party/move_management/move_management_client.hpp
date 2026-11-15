/* Move Management validated UDP input client. Source credits: docs/CREDITS.md.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace move_management {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

struct Packet {
    std::uint16_t version = 0;
    std::uint64_t sequence = 0;
    std::uint64_t sent_us = 0;
    bool valid = false;
    std::uint8_t pad[120]{};
    float optical_x = 0;
    float optical_y = 0;
    float optical_width = 0;
    float optical_height = 0;
    std::uint64_t optical_us = 0;
    bool optical_valid = false;
};

namespace detail {
inline std::uint16_t u16(const std::uint8_t *p) {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}
inline std::uint32_t u32(const std::uint8_t *p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
inline std::uint64_t u64(const std::uint8_t *p) {
    return std::uint64_t(u32(p)) | (std::uint64_t(u32(p + 4)) << 32);
}
inline float f32(const std::uint8_t *p) {
    const std::uint32_t bits = u32(p);
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}
inline bool finite3(const std::uint8_t *pad, std::size_t offset, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        if (!std::isfinite(f32(pad + offset + i * 4))) return false;
    return true;
}
} // namespace detail

class Client {
public:
    Client() = default;
    ~Client() { close(); }
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    bool open(std::uint16_t port = 9976) {
        close();
#if defined(_WIN32)
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
        winsock_started_ = true;
#endif
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ == kInvalidSocket) { close(); return false; }
#if defined(_WIN32)
        u_long nonblocking = 1;
        if (ioctlsocket(fd_, FIONBIO, &nonblocking) != 0) {
            close(); return false;
        }
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#if defined(__FreeBSD__)
        addr.sin_len = sizeof(addr);
#endif
        if (::bind(fd_, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close(); return false;
        }
        return true;
    }

    void close() {
        if (fd_ != kInvalidSocket) {
#if defined(_WIN32)
            closesocket(fd_);
#else
            ::close(fd_);
#endif
            fd_ = kInvalidSocket;
        }
#if defined(_WIN32)
        if (winsock_started_) { WSACleanup(); winsock_started_ = false; }
#endif
        packet_ = Packet{};
        have_packet_ = false;
        last_rx_us_ = 0;
        have_sequence_ = false;
        last_sequence_ = 0;
        have_peer_ = false;
    }

    void poll(std::uint64_t now_us) {
        if (fd_ == kInvalidSocket) return;
        const auto poll_started = std::chrono::steady_clock::now();
        std::uint8_t bytes[2048];
        sockaddr_in from{};
        for (unsigned drained = 0; drained < 64; ++drained) {
#if defined(_WIN32)
            int from_len = sizeof(from);
            const int n = recvfrom(fd_, reinterpret_cast<char *>(bytes),
                                   sizeof(bytes), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
            if (n == SOCKET_ERROR) break;
#else
            socklen_t from_len = sizeof(from);
            const ssize_t n = recvfrom(fd_, bytes, sizeof(bytes), MSG_DONTWAIT,
                                       reinterpret_cast<sockaddr *>(&from), &from_len);
            if (n < 0) break;
#endif
            if (from.sin_family != AF_INET || from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - poll_started).count();
            const std::uint64_t elapsed_us = static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed, 0));
            const std::uint64_t arrival_us = elapsed_us > std::numeric_limits<std::uint64_t>::max() - now_us
                ? std::numeric_limits<std::uint64_t>::max() : now_us + elapsed_us;
            Packet candidate{};
            if (!decode(bytes, static_cast<std::size_t>(n), arrival_us, candidate)) continue;
            if (have_sequence_ && arrival_us >= last_rx_us_ && arrival_us - last_rx_us_ <= 500000 &&
                candidate.sequence <= last_sequence_) continue;
            packet_ = candidate;
            peer_ = from;
            have_peer_ = true;
            have_packet_ = true;
            have_sequence_ = true;
            last_sequence_ = candidate.sequence;
            last_rx_us_ = now_us;
        }
    }

    bool active(std::uint64_t now_us) const {
        return have_packet_ && now_us >= last_rx_us_ && now_us - last_rx_us_ <= 500000;
    }
    bool fresh(std::uint64_t now_us) const {
        return active(now_us) && packet_.valid && detail::u32(packet_.pad + 76) == 1 &&
               now_us >= packet_.sent_us && now_us - packet_.sent_us <= 500000;
    }
    bool optical_fresh(std::uint64_t now_us) const {
        return fresh(now_us) && packet_.version == 2 && packet_.optical_valid &&
               now_us >= packet_.optical_us && now_us - packet_.optical_us <= 200000;
    }
    const Packet &packet() const { return packet_; }

    bool rumble(std::uint8_t strong, std::uint8_t weak, std::uint64_t now_us) {
        if (!have_peer_ || !fresh(now_us) || fd_ == kInvalidSocket) return false;
        std::uint8_t wire[32]{};
        auto put16 = [&](unsigned at, std::uint16_t n) {
            wire[at] = std::uint8_t(n); wire[at + 1] = std::uint8_t(n >> 8);
        };
        auto put64 = [&](unsigned at, std::uint64_t n) {
            for (unsigned i = 0; i < 8; ++i) wire[at + i] = std::uint8_t(n >> (i * 8));
        };
        wire[0] = 'M'; wire[1] = 'V'; wire[2] = 'R'; wire[3] = '1';
        put16(4, 1); put16(6, sizeof(wire));
        put64(8, packet_.sequence); put64(16, now_us);
        wire[24] = strong; wire[25] = weak; put16(26, 250);
#if defined(_WIN32)
        return ::sendto(fd_, reinterpret_cast<const char *>(wire), sizeof(wire), 0,
                        reinterpret_cast<const sockaddr *>(&peer_), sizeof(peer_)) == sizeof(wire);
#else
        return ::sendto(fd_, wire, sizeof(wire), 0,
                        reinterpret_cast<const sockaddr *>(&peer_), sizeof(peer_)) == sizeof(wire);
#endif
    }

private:
    static bool decode(const std::uint8_t *b, std::size_t n, std::uint64_t now_us, Packet &out) {
        if (n < 32 || detail::u32(b) != 0x3142564d) return false;
        const std::uint16_t version = detail::u16(b + 4);
        const std::size_t expected = version == 1 ? 152 : (version == 2 ? 184 : 0);
        if (!expected || n != expected || detail::u16(b + 6) != expected ||
            detail::u32(b + 28) != 0 || detail::u32(b + 24) > 1) return false;
        const std::uint64_t sent = detail::u64(b + 16);
        if (sent > now_us || now_us - sent > 500000) return false;
        const std::uint8_t *p = b + 32;
        const std::uint32_t connected = detail::u32(p + 76);
        if (connected > 1 || !detail::finite3(p, 12, 4) || !detail::finite3(p, 28, 3) ||
            !detail::finite3(p, 40, 3)) return false;
        out.version = version;
        out.sequence = detail::u64(b + 8);
        out.sent_us = sent;
        out.valid = detail::u32(b + 24) == 1;
        std::memcpy(out.pad, p, sizeof(out.pad));
        if (version == 2) {
            out.optical_x = detail::f32(b + 152);
            out.optical_y = detail::f32(b + 156);
            out.optical_width = detail::f32(b + 160);
            out.optical_height = detail::f32(b + 164);
            out.optical_us = detail::u64(b + 168);
            const std::uint32_t optical_valid = detail::u32(b + 176);
            if (detail::u32(b + 180) != 0 || optical_valid > 1) return false;
            out.optical_valid = optical_valid == 1;
            if (out.optical_us > sent) return false;
            if (out.optical_valid &&
                (!std::isfinite(out.optical_x) || !std::isfinite(out.optical_y) ||
                 !std::isfinite(out.optical_width) || !std::isfinite(out.optical_height) ||
                 out.optical_x < 0 || out.optical_x > 319 || out.optical_y < 0 || out.optical_y > 239 ||
                 out.optical_width < 1 || out.optical_width > 100 ||
                 out.optical_height < 1 || out.optical_height > 100)) return false;
        }
        return true;
    }

    SocketHandle fd_ = kInvalidSocket;
    sockaddr_in peer_{};
    bool have_peer_ = false;
    bool have_packet_ = false;
    bool have_sequence_ = false;
    std::uint64_t last_rx_us_ = 0;
    std::uint64_t last_sequence_ = 0;
    Packet packet_{};
#if defined(_WIN32)
    bool winsock_started_ = false;
#endif
};
} // namespace move_management
