// USB access to the camera: the role the Thesycon USBIO driver plays on Windows (docs/PROTOCOL.md 2.1).
#pragma once

#include "protocol/camera.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tir {

constexpr uint16_t kNaturalPointVendorId = 0x131D;

struct UsbDeviceInfo {
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    uint64_t registryId = 0;
    uint32_t locationId = 0;
    std::string product;
    std::string serial;
};

class UsbLink : public CommandSink {
public:
    static std::vector<UsbDeviceInfo> enumerate(uint16_t vendorId = kNaturalPointVendorId);
    static std::unique_ptr<UsbLink> open(const UsbDeviceInfo& device, std::string& error);

    ~UsbLink() override = default;

    // One completed IN transfer is one camera packet. Returns false on timeout.
    virtual bool waitPacket(std::vector<uint8_t>& packet, std::chrono::milliseconds timeout) = 0;
    virtual bool disconnected() const = 0;
    virtual std::string describeEndpoints() const = 0;
};

}  // namespace tir
