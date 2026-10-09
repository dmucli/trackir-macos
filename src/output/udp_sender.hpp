// opentrack "UDP over network" input format: six little-endian doubles x, y, z (cm), yaw, pitch, roll (deg).
#pragma once

#include "vision/pose.hpp"

#include <string>

namespace tir {

class UdpSender {
public:
    UdpSender() = default;
    ~UdpSender();
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    bool open(const std::string& host, int port, std::string& error);
    void send(const HeadPose& pose);

private:
    int fd_ = -1;
    unsigned char addr_[128] = {};
    unsigned addrLen_ = 0;
};

}  // namespace tir
