// The compute fixture not exercised by upstream's main probe: half sample + size.
#include "hakios-metal/command.hpp"
#include "hakios-metal/texture.hpp"
#include "../tests/fixtures/tex_size_h_air.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace hakios::metal;
// These inputs are zero or exact, positive binary16 normal values.
static uint16_t half(float f) {
    if (f == 0) return 0;
    uint32_t bits; std::memcpy(&bits, &f, sizeof(bits));
    return uint16_t((((bits >> 23) & 255) - 127 + 15) << 10 | ((bits & 0x7fffff) >> 13));
}
int main() {
    setenv("HAKIOS_METAL_AUDIT", "1", 1);
    std::string why;
    auto transport = openKernelGpu(&why);
    if (!transport) return 100;
    Gpu gpu(std::move(transport));
    auto queue = CommandQueue::make(gpu, &why);
    if (!queue) return 1;
    unsigned failures = 0;
    for (unsigned width : {4u, 8u}) {
        TextureDesc d; d.format=PixelFormat::kRGBA16Float; d.width=width; d.height=width; d.usage=kTextureUsageRead;
        auto in=Texture::make(gpu,d,&why);
        d.width=64; d.height=64; d.usage=kTextureUsageWrite;
        auto out=Texture::make(gpu,d,&why);
        if (!in || !out) return 1;
        std::vector<uint16_t> pixels(width*width*4);
        for(unsigned y=0;y<width;y++) for(unsigned x=0;x<width;x++) {
            unsigned i=(y*width+x)*4;
            pixels[i]=half(float(x)/8); pixels[i+1]=half(float(y)/8);
            pixels[i+2]=half(0.5f); pixels[i+3]=half(1.0f);
        }
        if (!in->replace(0,0,width,width,pixels.data(),width*8,&why)) return 1;
        std::memset(out->contents(),0xa5,out->bytesPerRow()*64);
        Sampler sampler; if(!makeSampler(SamplerDesc{},&sampler,&why)) return 1;
        Bindings b; b.textures={in.get(),out.get()}; b.samplers={&sampler};
        const uint32_t groups[3]={8,8,1}, local[3]={8,8,1};
        auto cb=queue->commandBuffer();
        if(!cb->dispatch(kernels::tex_size_h_air,groups,local,b)) return 1;
        cb->commit(); cb->waitUntilCompleted();
        pixels.resize(64*64*4);
        if(!out->read(0,0,64,64,pixels.data(),64*8,&why)) return 1;
        unsigned right=0;
        for(unsigned y=0;y<64;y++) for(unsigned x=0;x<64;x++) {
            unsigned i=(y*64+x)*4;
            right += pixels[i]==half(float((x*width)/64)*width/8) && pixels[i+1]==half(float((y*width)/64)*width/8) && pixels[i+2]==half(float(width)/2) && pixels[i+3]==half(float(width));
        }
        bool ok=right==4096 && cb->status()==CommandBuffer::kCompleted;
        std::printf("query: %s tex_size_h input=%ux%u output=64x64 right=%u/4096 error=%s\n",ok?"PASS":"FAIL",width,width,right,cb->error().c_str());
        failures+=!ok;
    }
    return failures ? 1 : 0;
}
