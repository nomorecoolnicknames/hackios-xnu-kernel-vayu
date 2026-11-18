// The probe on a host, against the fake GPU (fake_gpu.hpp): openKernelGpu as the phone's iokit_transport.cpp has it,
// answered by the fake instead of RedwoodGPU/HakiosGPU. Stages 1-4 run on the command processor alone and are carried
// out by the fake's interpreter; a dispatch or a draw is checked, not run (no shader instruction executes here).
#include "hakios-metal/fake_gpu.hpp"

namespace hakios::metal {

std::unique_ptr<Transport> openKernelGpu(std::string*) { return std::make_unique<FakeGpu>(true); }

}  // namespace hakios::metal
