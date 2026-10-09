// UsbLink on Apple's IOUSBHost framework. No kernel extension or third-party library is needed: the camera has a
// vendor-specific interface that no macOS driver claims.
#include "usb_link.hpp"

#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/IOMessage.h>
#import <IOUSBHost/IOUSBHost.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace tir {

namespace {

constexpr size_t kReadSize = 0x4000;  // the original keeps 4 reads of 16 KiB in flight
constexpr int kReadsInFlight = 4;

uint32_t registryNumber(io_service_t service, CFStringRef key)
{
    uint32_t value = 0;
    CFTypeRef ref = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    if (ref && CFGetTypeID(ref) == CFNumberGetTypeID())
        CFNumberGetValue(static_cast<CFNumberRef>(ref), kCFNumberSInt32Type, &value);
    if (ref)
        CFRelease(ref);
    return value;
}

std::string registryString(io_service_t service, CFStringRef key)
{
    std::string out;
    CFTypeRef ref = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    if (ref && CFGetTypeID(ref) == CFStringGetTypeID()) {
        char buf[256];
        if (CFStringGetCString(static_cast<CFStringRef>(ref), buf, sizeof(buf), kCFStringEncodingUTF8))
            out = buf;
    }
    if (ref)
        CFRelease(ref);
    return out;
}

std::string describe(NSError* error)
{
    return error ? std::string(error.localizedDescription.UTF8String) : std::string("unknown error");
}

io_service_t findInterfaceService(io_service_t device)
{
    io_iterator_t children = IO_OBJECT_NULL;
    if (IORegistryEntryGetChildIterator(device, kIOServicePlane, &children) != KERN_SUCCESS)
        return IO_OBJECT_NULL;
    io_service_t found = IO_OBJECT_NULL;
    while (io_service_t child = IOIteratorNext(children)) {
        if (!found && IOObjectConformsTo(child, "IOUSBHostInterface") &&
            registryNumber(child, CFSTR("bInterfaceNumber")) == 0) {
            found = child;
            continue;
        }
        IOObjectRelease(child);
    }
    IOObjectRelease(children);
    return found;
}

class IOUSBHostLink final : public UsbLink {
public:
    ~IOUSBHostLink() override { close(); }

    bool open(const UsbDeviceInfo& info, std::string& error)
    {
        queue_ = dispatch_queue_create("trackir.usb", DISPATCH_QUEUE_SERIAL);
        io_service_t deviceService = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(info.registryId));
        if (!deviceService) {
            error = "device disappeared";
            return false;
        }

        auto interest = ^(IOUSBHostObject*, uint32_t messageType, void*) {
            if (messageType == kIOMessageServiceIsTerminated)
                gone_ = true;
        };

        NSError* err = nil;
        device_ = [[IOUSBHostDevice alloc] initWithIOService:deviceService options:IOUSBHostObjectInitOptionsNone
                                                       queue:queue_ error:&err interestHandler:interest];
        if (!device_) {
            IOObjectRelease(deviceService);
            error = "cannot open device: " + describe(err);
            return false;
        }

        // The original opens configuration 1, interface 0 (OpenUsbioDeviceAndBindPipes, 005ca300).
        if (device_.configurationDescriptor == nullptr && ![device_ configureWithValue:1 error:&err]) {
            IOObjectRelease(deviceService);
            error = "cannot select configuration 1: " + describe(err);
            return false;
        }

        io_service_t interfaceService = IO_OBJECT_NULL;
        for (int attempt = 0; attempt < 40 && !interfaceService; attempt++) {
            interfaceService = findInterfaceService(deviceService);
            if (!interfaceService)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        IOObjectRelease(deviceService);
        if (!interfaceService) {
            error = "interface 0 did not appear";
            return false;
        }

        interface_ = [[IOUSBHostInterface alloc] initWithIOService:interfaceService options:IOUSBHostObjectInitOptionsNone
                                                             queue:queue_ error:&err interestHandler:interest];
        if (!interface_) {
            interface_ = [[IOUSBHostInterface alloc] initWithIOService:interfaceService
                                                               options:IOUSBHostObjectInitOptionsDeviceSeize
                                                                 queue:queue_ error:&err interestHandler:interest];
        }
        IOObjectRelease(interfaceService);
        if (!interface_) {
            error = "cannot open interface 0: " + describe(err);
            return false;
        }

        if (!findEndpoints(error))
            return false;

        outPipe_ = [interface_ copyPipeWithAddress:outAddress_ error:&err];
        inPipe_ = outPipe_ ? [interface_ copyPipeWithAddress:inAddress_ error:&err] : nil;
        if (!outPipe_ || !inPipe_) {
            error = "cannot open pipes: " + describe(err);
            return false;
        }

        for (int i = 0; i < kReadsInFlight; i++) {
            // ioData buffers come back at full length and throw if resized, so never touch .length on them.
            NSMutableData* buffer = [interface_ ioDataWithCapacity:kReadSize error:&err];
            if (!buffer)
                buffer = [NSMutableData dataWithLength:kReadSize];
            enqueueRead(buffer);
        }
        return true;
    }

    bool send(const uint8_t* data, size_t length) override
    {
        if (gone_ || !outPipe_)
            return false;
        NSMutableData* d = [NSMutableData dataWithBytes:data length:length];
        NSUInteger transferred = 0;
        NSError* err = nil;
        BOOL ok = [outPipe_ sendIORequestWithData:d bytesTransferred:&transferred completionTimeout:1.0 error:&err];
        return ok && transferred == length;
    }

    bool waitPacket(std::vector<uint8_t>& packet, std::chrono::milliseconds timeout) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [&] { return !packets_.empty() || gone_; }) || packets_.empty())
            return false;
        packet = std::move(packets_.front());
        packets_.pop_front();
        return true;
    }

    bool disconnected() const override { return gone_; }

    std::string describeEndpoints() const override
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "OUT 0x%02x, IN 0x%02x", outAddress_, inAddress_);
        return buf;
    }

private:
    // pipe[0] is the command writer and pipe[1] the reader in the original; take the first OUT and first IN
    // bulk/interrupt endpoints of interface 0.
    bool findEndpoints(std::string& error)
    {
        const IOUSBConfigurationDescriptor* config = interface_.configurationDescriptor;
        const IOUSBInterfaceDescriptor* iface = interface_.interfaceDescriptor;
        const IOUSBEndpointDescriptor* ep = nullptr;
        while ((ep = IOUSBGetNextEndpointDescriptor(config, iface, reinterpret_cast<const IOUSBDescriptorHeader*>(ep)))) {
            uint8_t type = ep->bmAttributes & 0x03;
            if (type != 2 && type != 3)
                continue;
            if ((ep->bEndpointAddress & 0x80) && !inAddress_)
                inAddress_ = ep->bEndpointAddress;
            else if (!(ep->bEndpointAddress & 0x80) && !outAddress_)
                outAddress_ = ep->bEndpointAddress;
        }
        if (!inAddress_ || !outAddress_) {
            error = "interface 0 lacks a bulk IN/OUT endpoint pair";
            return false;
        }
        return true;
    }

    void enqueueRead(NSMutableData* buffer)
    {
        if (gone_ || closing_)
            return;
        NSError* err = nil;
        BOOL ok = [inPipe_ enqueueIORequestWithData:buffer completionTimeout:0 error:&err
                                  completionHandler:^(IOReturn status, NSUInteger transferred) {
                                      if (status == kIOReturnSuccess && transferred > 0) {
                                          const uint8_t* bytes = static_cast<const uint8_t*>(buffer.bytes);
                                          {
                                              std::lock_guard<std::mutex> lock(mutex_);
                                              if (packets_.size() < 256)
                                                  packets_.emplace_back(bytes, bytes + transferred);
                                          }
                                          cv_.notify_one();
                                      } else if (status == kIOReturnAborted || status == kIOReturnNoDevice ||
                                                 status == kIOReturnNotResponding) {
                                          if (status != kIOReturnAborted)
                                              gone_ = true;
                                          cv_.notify_all();
                                          return;
                                      }
                                      enqueueRead(buffer);
                                  }];
        if (!ok) {
            gone_ = true;
            cv_.notify_all();
        }
    }

    void close()
    {
        closing_ = true;
        NSError* err = nil;
        if (inPipe_)
            [inPipe_ abortWithError:&err];
        if (outPipe_)
            [outPipe_ abortWithError:&err];
        if (interface_)
            [interface_ destroy];
        if (device_)
            [device_ destroy];
        if (queue_)
            dispatch_sync(queue_, ^{});  // let aborted completions finish before members go away
        inPipe_ = nil;
        outPipe_ = nil;
        interface_ = nil;
        device_ = nil;
    }

    dispatch_queue_t queue_ = nil;
    IOUSBHostDevice* device_ = nil;
    IOUSBHostInterface* interface_ = nil;
    IOUSBHostPipe* outPipe_ = nil;
    IOUSBHostPipe* inPipe_ = nil;
    uint8_t outAddress_ = 0, inAddress_ = 0;
    std::atomic<bool> gone_{false};
    std::atomic<bool> closing_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> packets_;
};

}  // namespace

std::vector<UsbDeviceInfo> UsbLink::enumerate(uint16_t vendorId)
{
    std::vector<UsbDeviceInfo> out;
    @autoreleasepool {
        // IOUSBHostDevice applies USB matching rules, under which idVendor alone matches nothing (it needs
        // idProduct too). Match the class and filter the vendor here instead.
        io_iterator_t it = IO_OBJECT_NULL;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &it) != KERN_SUCCESS)
            return out;
        while (io_service_t service = IOIteratorNext(it)) {
            UsbDeviceInfo info;
            info.vendorId = uint16_t(registryNumber(service, CFSTR("idVendor")));
            if (info.vendorId != vendorId) {
                IOObjectRelease(service);
                continue;
            }
            info.productId = uint16_t(registryNumber(service, CFSTR("idProduct")));
            info.locationId = registryNumber(service, CFSTR("locationID"));
            info.product = registryString(service, CFSTR("USB Product Name"));
            info.serial = registryString(service, CFSTR("USB Serial Number"));
            IORegistryEntryGetRegistryEntryID(service, &info.registryId);
            out.push_back(info);
            IOObjectRelease(service);
        }
        IOObjectRelease(it);
    }
    return out;
}

std::unique_ptr<UsbLink> UsbLink::open(const UsbDeviceInfo& device, std::string& error)
{
    @autoreleasepool {
        auto link = std::make_unique<IOUSBHostLink>();
        if (!link->open(device, error))
            return nullptr;
        return link;
    }
}

}  // namespace tir
