#include "udp_sender.hpp"

#include <cstring>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tir {

UdpSender::~UdpSender()
{
    if (fd_ >= 0)
        close(fd_);
}

bool UdpSender::open(const std::string& host, int port, std::string& error)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0 || !res) {
        error = "cannot resolve " + host + ": " + gai_strerror(rc);
        return false;
    }
    fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd_ < 0 || res->ai_addrlen > sizeof(addr_)) {
        error = "cannot create UDP socket";
        freeaddrinfo(res);
        return false;
    }
    std::memcpy(addr_, res->ai_addr, res->ai_addrlen);
    addrLen_ = unsigned(res->ai_addrlen);
    freeaddrinfo(res);
    return true;
}

void UdpSender::send(const HeadPose& p)
{
    if (fd_ < 0)
        return;
    double values[6] = {p.x, p.y, p.z, p.yaw, p.pitch, p.roll};
    sendto(fd_, values, sizeof(values), 0, reinterpret_cast<sockaddr*>(addr_), addrLen_);
}

}  // namespace tir
