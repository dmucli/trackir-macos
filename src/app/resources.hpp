// Vendor blobs the cameras need, extracted from the user's own TrackIR5.exe (never shipped with the port).
#pragma once

#include "protocol/camera.hpp"

#include <string>
#include <vector>

namespace tir {

struct VendorBlob {
    const char* fileName;
    const char* description;
    uint32_t virtualAddress;  // in TrackIR5.exe 5.5.3
    uint32_t size;
    const char* sha256;
};

const std::vector<VendorBlob>& vendorBlobs();

// ~/Library/Application Support/TrackIR-macOS
std::string resourceDirectory();

// Reads TrackIR5.exe, locates each blob through the PE section table, verifies its SHA-256 and writes it to `outDir`.
bool extractVendorBlobs(const std::string& exePath, const std::string& outDir, std::vector<std::string>& messages);

// Loads what a given camera needs from resourceDirectory().
DriverResources loadDriverResources(const CameraModel& model, std::vector<std::string>& messages);

}  // namespace tir
