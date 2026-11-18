// The kernel's GPU driver (HakiosGPU) through its IOKit user client: the requests as struct-method calls whose
// selector is the request's number, and a buffer's memory as the client memory of the type that is its handle. The
// same as hakios's tools/mesa/hakios-drm/hakios.drm.c does for freedreno.
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <set>

#include "hakios-metal/gpu.hpp"

namespace hakios::metal {

namespace {

class KernelGpu : public Transport {
public:
    explicit KernelGpu(io_connect_t c) : c_(c), audit_(getenv("HAKIOS_METAL_AUDIT") != nullptr) {
        mach_timebase_info(&timebase_);
    }
    ~KernelGpu() override {
        kern_return_t r = IOServiceClose(c_);
        if (audit_) {
            fprintf(stderr, "transport-audit: %s handles=%zu mappings=%zu queues=%zu calls=%u errors=%u close=%#x first_iova=%#llx\n",
                    handles_.empty() && mappings_.empty() && queues_.empty() && errors_ == 0 && r == KERN_SUCCESS ? "PASS" : "FAIL",
                    handles_.size(), mappings_.size(), queues_.size(), calls_, errors_, unsigned(r),
                    static_cast<unsigned long long>(firstIova_));
        }
    }

    int call(uint32_t request, void* data, size_t size) override {
        size_t returned = size;
        kern_return_t r = IOConnectCallStructMethod(c_, request, data, size, data, &returned);
        if (audit_) {
            calls_++;
            if (r != KERN_SUCCESS) {
                errors_++;
                // (each failed request by name, so that a deliberate refusal a test asks for is told from a fault)
                fprintf(stderr, "transport-audit: call %u request %u answered %#x\n", calls_, request, unsigned(r));
            } else if (request == kGemNew) handles_.insert(static_cast<MsmGemNew*>(data)->handle);
            else if (request == kGemClose) handles_.erase(static_cast<DrmGemClose*>(data)->handle);
            else if (request == kSubmitQueueNew) queues_.insert(static_cast<MsmSubmitQueue*>(data)->id);
            else if (request == kSubmitQueueClose) queues_.erase(*static_cast<uint32_t*>(data));
            else if (request == kGemInfo && firstIova_ == 0 && static_cast<MsmGemInfo*>(data)->info == kInfoGetIova)
                firstIova_ = static_cast<MsmGemInfo*>(data)->value;
        }
        switch (r) {
        case KERN_SUCCESS: return 0;
        case kIOReturnNoMemory: return ENOMEM;
        case kIOReturnBadArgument: return EINVAL;
        case kIOReturnTimeout: return ETIMEDOUT;
        case kIOReturnBusy: return EBUSY;
        case kIOReturnNotFound: return ENOENT;
        case kIOReturnUnsupported: return ENOSYS;
        default:
            // the kernel's own answer would be lost in a plain EIO: a code no case names is a failure nobody has
            // looked at yet, so it is printed where it happens
            fprintf(stderr, "hakios-metal: the driver's request %u answered %#x\n", request, unsigned(r));
            return EIO;
        }
    }

    void* map(uint32_t handle, uint64_t) override {
        mach_vm_address_t address = 0;
        mach_vm_size_t size = 0;
        // as the driver made the memory: write-combined, the way Linux hands it to freedreno
        if (IOConnectMapMemory64(c_, handle, mach_task_self(), &address, &size, kIOMapAnywhere) != KERN_SUCCESS) {
            if (audit_) errors_++;
            return nullptr;
        }
        if (audit_) mappings_.insert(address);
        return reinterpret_cast<void*>(uintptr_t(address));
    }

    void unmap(uint32_t handle, void* address) override {
        kern_return_t r = IOConnectUnmapMemory64(c_, handle, mach_task_self(), mach_vm_address_t(uintptr_t(address)));
        if (audit_) {
            if (r != KERN_SUCCESS || mappings_.erase(uintptr_t(address)) != 1) errors_++;
        }
    }

    int64_t nowNs() override {
        // the driver's deadlines are of mach_continuous_time (drivers/gpu/hakios.gpu.cpp, millisecondsUntil)
        return int64_t(mach_continuous_time() * timebase_.numer / timebase_.denom);
    }

private:
    io_connect_t c_;
    mach_timebase_info_data_t timebase_{};
    bool audit_;
    std::set<uint32_t> handles_, queues_;
    std::set<uint64_t> mappings_;
    uint32_t calls_ = 0, errors_ = 0;
    uint64_t firstIova_ = 0;
};

}  // namespace

// The service: hakios's kernel calls it HakiosGPU; the vayu kernel (iokit/Kernel/RedwoodGPU.cpp, started by
// bsd/dev/vayu_adreno.c gpu_keep() with boot-args gpu=1 vayu_gpu=1 vayu_keep=1) calls the same msm DRM interface
// RedwoodGPU. HAKIOS_METAL_SERVICE names another.
std::unique_ptr<Transport> openKernelGpu(std::string* why) {
    const char* wanted = getenv("HAKIOS_METAL_SERVICE");
    const char* const names[] = {wanted != nullptr && *wanted != 0 ? wanted : "HakiosGPU", "RedwoodGPU"};
    io_service_t s = 0;
    const char* name = nullptr;
    for (const char* n : names) {
        s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(n));
        if (s != 0) {
            name = n;
            break;
        }
    }
    if (s == 0) {
        *why = std::string("no ") + names[0] + " or RedwoodGPU service: the kernel has not started the GPU (gpu=1 vayu_gpu=1 vayu_keep=1, "
               "the firmware, the phone)";
        return nullptr;
    }
    io_connect_t c = 0;
    kern_return_t r = IOServiceOpen(s, mach_task_self(), 0, &c);
    IOObjectRelease(s);
    if (r != KERN_SUCCESS) {
        *why = std::string(name) + " would not open: error " + std::to_string(r);
        return nullptr;
    }
    fprintf(stderr, "hakios-metal: the GPU through the service %s\n", name);
    return std::unique_ptr<Transport>(new KernelGpu(c));
}

}  // namespace hakios::metal
