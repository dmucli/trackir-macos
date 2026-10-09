#include "np_bridge.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tir {

static_assert(sizeof(tir_bridge) <= TIR_BRIDGE_FILE_SIZE, "bridge must fit in one page");

namespace {

float scaled(double value, double fullScale)
{
    double v = value / fullScale * NP_AXIS_LIMIT;
    return float(std::clamp(v, double(-NP_AXIS_LIMIT), double(NP_AXIS_LIMIT)));
}

uint32_t msvcRand()
{
    return arc4random() & 0x7FFF;
}

}  // namespace

np_trackir_data toTrackIRData(const HeadPose& pose)
{
    np_trackir_data d{};
    d.yaw = scaled(pose.yaw, 180.0);
    d.pitch = scaled(-pose.pitch, 180.0);
    d.roll = scaled(pose.roll, 180.0);
    d.x = scaled(-pose.x, 50.0);
    d.y = scaled(pose.y, 50.0);
    d.z = scaled(pose.z, 50.0);
    d.status = 0;
    return d;
}

NpBridge::NpBridge(std::string path) : path_(std::move(path)) {}

NpBridge::~NpBridge()
{
    if (bridge_) {
        bridge_->daemon_pid = 0;
        bridge_->tracking = 0;
        bridge_->pose_flags = 0;
        munmap(bridge_, TIR_BRIDGE_FILE_SIZE);
    }
    if (fd_ >= 0)
        close(fd_);
}

bool NpBridge::open(std::string& error)
{
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd_ < 0) {
        error = "cannot open " + path_ + ": " + std::strerror(errno);
        return false;
    }
    fchmod(fd_, 0666);  // games may run as another user under some Wine setups
    if (ftruncate(fd_, TIR_BRIDGE_FILE_SIZE) != 0) {
        error = "cannot size " + path_ + ": " + std::strerror(errno);
        return false;
    }
    void* mem = mmap(nullptr, TIR_BRIDGE_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mem == MAP_FAILED) {
        error = "cannot map " + path_ + ": " + std::strerror(errno);
        return false;
    }
    bridge_ = static_cast<tir_bridge*>(mem);

    // Keep an existing command ring position so clients that are already running stay in sync.
    bool fresh = bridge_->magic != TIR_BRIDGE_MAGIC || bridge_->version != TIR_BRIDGE_VERSION;
    if (fresh) {
        std::memset(bridge_, 0, TIR_BRIDGE_FILE_SIZE);
        np_block_init_dll_defaults(&bridge_->block);
    }
    cmdTail_ = bridge_->cmd_head;
    np_block_init_trackir(&bridge_->block);
    bridge_->daemon_pid = getpid();
    bridge_->version = TIR_BRIDGE_VERSION;
    __atomic_store_n(&bridge_->magic, TIR_BRIDGE_MAGIC, __ATOMIC_RELEASE);
    return true;
}

bool NpBridge::loadGameKeys(const std::string& csvPath, std::string& error)
{
    std::ifstream f(csvPath);
    if (!f) {
        error = "cannot open " + csvPath;
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::replace(line.begin(), line.end(), ';', ',');
        std::istringstream in(line);
        std::string id, key;
        if (!std::getline(in, id, ',') || !std::getline(in, key, ','))
            continue;
        key.erase(std::remove_if(key.begin(), key.end(), ::isspace), key.end());
        if (key.size() != 16)
            continue;
        std::array<uint8_t, 8> bytes{};
        for (int i = 0; i < 8; i++)
            bytes[size_t(i)] = uint8_t(std::stoul(key.substr(size_t(2 * i), 2), nullptr, 16));
        keys_[uint32_t(std::stoul(id, nullptr, 0))] = bytes;
    }
    return true;
}

void NpBridge::pollCommands()
{
    if (!bridge_)
        return;
    uint32_t head = __atomic_load_n(&bridge_->cmd_head, __ATOMIC_ACQUIRE);
    if (head - cmdTail_ > TIR_BRIDGE_RING)
        cmdTail_ = head - TIR_BRIDGE_RING;  // a client flooded the ring; drop the oldest
    while (cmdTail_ != head) {
        tir_bridge_command& c = bridge_->cmds[cmdTail_ % TIR_BRIDGE_RING];
        if (__atomic_load_n(&c.sequence, __ATOMIC_ACQUIRE) != cmdTail_ + 1)
            break;  // claimed but not written yet
        uint32_t code = c.code, arg = c.arg;
        cmdTail_++;
        switch (code) {
        case NP_CMD_REGISTER_PROFILE_ID:
            profileId_ = arg;
            if (events_.profileRegistered)
                events_.profileRegistered(arg);
            break;
        case NP_CMD_START_TRANSMISSION:
        case NP_CMD_STOP_TRANSMISSION:
            transmitting_ = code == NP_CMD_START_TRANSMISSION;
            if (events_.transmissionChanged)
                events_.transmissionChanged(transmitting_);
            break;
        case NP_CMD_RECENTER:
            if (events_.recenter)
                events_.recenter();
            break;
        default:
            break;
        }
    }
}

void NpBridge::publish(const HeadPose& pose, bool tracking)
{
    if (!bridge_)
        return;
    np_trackir_data d = toTrackIRData(pose);
    if (transmitting_ && ++frameSignature_ > 0x7FFE)
        frameSignature_ = 1;
    d.frame_signature = frameSignature_;

    auto key = keys_.find(profileId_);
    if (key != keys_.end())
        np_data_encrypt(&d, key->second.data(), msvcRand);

    __atomic_add_fetch(&bridge_->data_seq, 1, __ATOMIC_ACQ_REL);
    std::memcpy(&bridge_->block.data, &d, sizeof(d));
    bridge_->tracking = tracking ? 1 : 0;
    __atomic_add_fetch(&bridge_->data_seq, 1, __ATOMIC_ACQ_REL);
    heartbeat();
}

void NpBridge::publishNative(const HeadPose& pose, uint32_t flags)
{
    if (!bridge_)
        return;
    __atomic_add_fetch(&bridge_->pose_seq, 1, __ATOMIC_ACQ_REL);
    const double values[6] = {pose.yaw, pose.pitch, pose.roll, pose.x, pose.y, pose.z};
    for (int i = 0; i < 6; i++)
        bridge_->pose[i] = values[i];
    bridge_->pose_flags = flags;
    __atomic_add_fetch(&bridge_->pose_seq, 1, __ATOMIC_ACQ_REL);
}

void NpBridge::heartbeat()
{
    if (bridge_)
        __atomic_add_fetch(&bridge_->heartbeat, 1, __ATOMIC_RELEASE);
}

}  // namespace tir
