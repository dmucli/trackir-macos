#include "resources.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include <stdlib.h>
#include <sys/stat.h>

namespace tir {

const std::vector<VendorBlob>& vendorBlobs()
{
    // GetEmbeddedResourceBlob (0059d670) resources 5, 9, 0x12 and the Rev35 key table (Rev35_BuildHandshakePacket).
    static const std::vector<VendorBlob> blobs = {
        {"fpga_rev5.bin", "TrackIR 4 FPGA image (resource 5)", 0x008EB9A0, 0x11C29,
         "d6ffe0f3de53908363af54b3f6d0b34e6dd6cbc10e8f460bffeb3df299096ec4"},
        {"fpga_rev9.bin", "TrackIR 5 Rev9 FPGA image (resource 9)", 0x00938750, 0x8CEC,
         "5fd3028f8556edd6a5309acc95220e3375c6d58a8f2515f37b963cc430ff79a7"},
        {"fpga_rev18.bin", "TrackIR 5 Rev18 FPGA image (resource 0x12)", 0x00D51AB8, 0x8D31,
         "8e64e7693c7b99fd1e91d93e67755f62a9851c3a0d6627973c942fffa83146f3"},
        {"rev35_keys.bin", "TrackIR 5 Rev35 camera authentication keys", 0x01850D80, 128,
         "b36e36c5de8716c6c563063f9664bbdbe3afa3d94ecd933e45416f1014db0f74"},
    };
    return blobs;
}

std::string resourceDirectory()
{
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/Library/Application Support/TrackIR-macOS";
}

namespace {

uint32_t le32(const std::vector<uint8_t>& d, size_t off)
{
    return off + 4 <= d.size() ? uint32_t(d[off]) | uint32_t(d[off + 1]) << 8 | uint32_t(d[off + 2]) << 16 |
                                     uint32_t(d[off + 3]) << 24
                               : 0;
}

uint16_t le16(const std::vector<uint8_t>& d, size_t off)
{
    return off + 2 <= d.size() ? uint16_t(d[off] | d[off + 1] << 8) : 0;
}

std::string sha256Hex(const uint8_t* data, size_t len)
{
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, CC_LONG(len), digest);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char c : digest) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 15]);
    }
    return out;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Maps a virtual address to a file offset through the PE32 section table. Returns false if unmapped.
bool vaToFileOffset(const std::vector<uint8_t>& pe, uint32_t va, uint32_t size, size_t& offset)
{
    if (pe.size() < 0x40 || pe[0] != 'M' || pe[1] != 'Z')
        return false;
    uint32_t peOff = le32(pe, 0x3C);
    if (le32(pe, peOff) != 0x00004550)
        return false;
    uint16_t sections = le16(pe, peOff + 6);
    uint16_t optSize = le16(pe, peOff + 20);
    size_t opt = peOff + 24;
    if (le16(pe, opt) != 0x10B)  // PE32
        return false;
    uint32_t imageBase = le32(pe, opt + 28);
    uint32_t rva = va - imageBase;
    size_t table = opt + optSize;
    for (uint16_t i = 0; i < sections; i++) {
        size_t s = table + size_t(i) * 40;
        uint32_t vsize = le32(pe, s + 8), vaddr = le32(pe, s + 12), rawSize = le32(pe, s + 16), rawPtr = le32(pe, s + 20);
        if (rva >= vaddr && rva + size <= vaddr + std::max(vsize, rawSize) && rva - vaddr + size <= rawSize) {
            offset = rawPtr + (rva - vaddr);
            return offset + size <= pe.size();
        }
    }
    return false;
}

}  // namespace

bool extractVendorBlobs(const std::string& exePath, const std::string& outDir, std::vector<std::string>& messages)
{
    std::vector<uint8_t> exe;
    if (!readFile(exePath, exe)) {
        messages.push_back("cannot read " + exePath);
        return false;
    }
    mkdir((outDir.substr(0, outDir.rfind('/'))).c_str(), 0755);
    mkdir(outDir.c_str(), 0755);

    bool allOk = true;
    for (const VendorBlob& blob : vendorBlobs()) {
        size_t off = 0;
        if (!vaToFileOffset(exe, blob.virtualAddress, blob.size, off)) {
            messages.push_back(std::string(blob.fileName) + ": address not found (different TrackIR version?)");
            allOk = false;
            continue;
        }
        std::string hash = sha256Hex(exe.data() + off, blob.size);
        if (hash != blob.sha256) {
            messages.push_back(std::string(blob.fileName) + ": checksum mismatch; this extractor knows TrackIR 5.5.3 only");
            allOk = false;
            continue;
        }
        std::string path = outDir + "/" + blob.fileName;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(exe.data() + off), blob.size);
        messages.push_back(std::string("wrote ") + path + " (" + blob.description + ")");
    }
    return allOk;
}

DriverResources loadDriverResources(const CameraModel& model, std::vector<std::string>& messages)
{
    DriverResources r;
    std::string dir = resourceDirectory();
    if (model.fpgaResource >= 0) {
        std::string name = model.fpgaResource == 9 ? "fpga_rev9.bin" : model.fpgaResource == 0x12 ? "fpga_rev18.bin" : "fpga_rev5.bin";
        if (!readFile(dir + "/" + name, r.fpgaImage))
            messages.push_back("missing " + dir + "/" + name + "; run: trackir-mac extract-fpga /path/to/TrackIR5.exe");
    }
    if (model.protocol == ProtocolKind::Secure) {
        std::vector<uint8_t> keys;
        if (readFile(dir + "/rev35_keys.bin", keys) && keys.size() == sizeof(r.keyTable)) {
            std::memcpy(r.keyTable, keys.data(), sizeof(r.keyTable));
            r.haveKeyTable = true;
        }
    }
    return r;
}

}  // namespace tir
