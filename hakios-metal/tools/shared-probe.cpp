// Focused task-6 shared-memory regression and actual submission capture.
#include "hakios-metal/command.hpp"
#include "../tests/fixtures/tg_arg_air.hpp"
#include <cstdio>
#include <cstdlib>
using namespace hakios::metal;
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    setenv("HAKIOS_METAL_AUDIT", "1", 1);
    std::string why;
    auto transport = openKernelGpu(&why);
    if (!transport) return 100;
    Gpu gpu(std::move(transport));
    auto q = CommandQueue::make(gpu, &why);
    auto out = newBuffer(gpu, 8 * 1024 + 256, &why);
    if (!q || !out) return 1;
    auto* w = static_cast<uint32_t*>(out->contents());
    for (unsigned i = 0; i < 2112; i++) w[i] = 0x5a5a5a5a;
    const uint32_t groups[3] = {16, 1, 1}, local[3] = {64, 1, 1};
    Bindings b;
    b.buffers = {Binding{out.get(), 0}};
    b.sharedBytes = kernels::tg_arg_air.sharedSize + 8192;
    auto cb = q->commandBuffer();
    if (!cb->dispatch(kernels::tg_arg_air, groups, local, b)) return 1;
    setenv("HAKIOS_METAL_RD", argv[1], 1);
    cb->commit(); cb->waitUntilCompleted();
    unsetenv("HAKIOS_METAL_RD");
    unsigned right = 0, tail = 0;
    for (unsigned g = 0; g < 1024; g++) {
        unsigned n = (g + 1) & 63;
        if (w[2*g] == n+1000 && w[2*g+1] == n+2000) right++;
        else if (g < 64) std::printf("shared: gid=%u got=(%u,%u) want=(%u,%u)\n", g,w[2*g],w[2*g+1],n+1000,n+2000);
    }
    for (unsigned i = 2048; i < 2112; i++) tail += w[i] == 0x5a5a5a5a;
    std::printf("shared: %s right=%u/1024 tail=%u/64 status=%d error=%s\n", right==1024 && tail==64 && cb->status()==CommandBuffer::kCompleted ? "PASS" : "FAIL",right,tail,int(cb->status()),cb->error().c_str());
    return right==1024 && tail==64 && cb->status()==CommandBuffer::kCompleted ? 0 : 1;
}
