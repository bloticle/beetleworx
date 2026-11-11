#include "character_timing.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <immintrin.h>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_827874B0);
bool em2::TestNativeCharacterTiming() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Release {uint8_t* base;unsigned csr=_mm_getcsr();~Release(){_mm_setcsr(csr);if(base)VirtualFree(base,0,MEM_RELEASE);}} release{base};
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"private/image.bin";
    const auto image_size=std::filesystem::file_size(path);
    if(!base || !VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x82000000,image_size,MEM_COMMIT,PAGE_READWRITE))return false;
    std::ifstream image(path,std::ios::binary);
    if(!image.read(reinterpret_cast<char*>(base+0x82000000),image_size))return false;
    auto put=[&](uint32_t address,float value){PPC_STORE_U32(address,std::bit_cast<uint32_t>(value));};
    auto get=[&](uint32_t address){return std::bit_cast<float>(PPC_LOAD_U32(address));};
    constexpr uint32_t controller=0x1000,entity=0x2000,direction=0x3000;
    unsigned steps=0,scenarios=0;
    for(unsigned hz:{30u,40u,60u,90u,120u,240u})for(unsigned mode:{1u,2u})
      for(bool scaled:{false,true})for(bool paused:{false,true}) {
        std::memset(base,0,0x10000);
        PPC_STORE_U32(controller+48,entity);PPC_STORE_U32(controller+568,30);
        PPC_STORE_U32(controller+872,mode);put(controller+876,5);
        put(controller+608,10);put(controller+616,10);put(controller+624,1);
        PPC_STORE_U8(entity+92,scaled);PPC_STORE_U8(entity+93,paused);put(direction,1);
        const float dt=1.f/hz,effective=paused?0:scaled?dt*.5f:dt;
        float expected=0;
        for(unsigned frame=0;frame<hz*2;++frame) {
            const float target=frame<hz?5.f:0.f;
            put(0x833FCA54,dt);put(0x833FCA70,dt*.5f);
            PPCContext context{};context.r1.u64=0xF000;context.r3.u64=controller;context.r6.u64=direction;
            context.f1.f64=target?-1:0;context.f2.f64=0;context.fpscr.InitHost();
            __imp__sub_827874B0(context,base);++steps;
            expected=target>expected?std::min(target,expected+10*effective):std::max(target,expected-10*effective);
            if(!std::isfinite(get(controller+144)) || !std::isfinite(get(controller+148)) ||
               std::abs(get(controller+144)-expected)>2e-4f || std::abs(get(controller+148))>2e-4f ||
               get(controller+176)!=1 || get(controller+188)!=0) {
                REXLOG_ERROR("Native character acceleration mismatch: {} Hz, mode {}, scaled {}, paused {}, frame {}, velocity {}/{}, expected {}",hz,mode,scaled,paused,frame,get(controller+144),get(controller+148),expected);
                return false;
            }
        }
        ++scenarios;
    }
    // Straight acceleration never exercises mode 2's angular path. Execute
    // its real atan2/shortest-angle/sin/cos helpers while holding a new heading,
    // including uneven render intervals, time scaling and paused entities.
    unsigned turn_steps=0,turn_scenarios=0;
    for(unsigned hz:{30u,60u,120u,240u,0u})for(unsigned mode:{1u,2u})
      for(bool scaled:{false,true})for(bool paused:{false,true})for(double angle:{.5*3.141592653589793,.75*3.141592653589793}) {
        std::memset(base,0,0x10000);
        PPC_STORE_U32(controller+48,entity);PPC_STORE_U32(controller+568,30);
        PPC_STORE_U32(controller+872,mode);put(controller+876,5);
        put(controller+608,10);put(controller+616,10);put(controller+624,1);
        PPC_STORE_U8(entity+92,scaled);PPC_STORE_U8(entity+93,paused);put(direction,1);
        put(controller+144,5);
        const float input_x=float(-std::cos(angle)),input_y=float(std::sin(angle));
        const float target_x=-float(5*input_x),target_y=float(5*input_y);
        const double dx=double(target_x)-5,dy=target_y,distance=std::hypot(dx,dy);
        constexpr std::array uneven={1./240,1./40,1./90,1./60};
        double elapsed=0,effective=0;unsigned frame=0;
        while(elapsed<1) {
            const double interval=std::min(1-elapsed,hz?1./hz:uneven[frame%uneven.size()]);
            const float dt=float(interval);elapsed+=interval;effective+=paused?0:scaled?double(dt)*.5:dt;
            put(0x833FCA54,dt);put(0x833FCA70,dt*.5f);
            PPCContext context{};context.r1.u64=0xF000;context.r3.u64=controller;context.r6.u64=direction;
            context.f1.f64=input_x;context.f2.f64=input_y;context.fpscr.InitHost();
            __imp__sub_827874B0(context,base);++turn_steps;++frame;
            const double fraction=std::min(1.,10*effective/distance);
            const double heading=std::min(angle,effective);
            const double expected_x=mode==1?5+dx*fraction:5*std::cos(heading);
            const double expected_y=mode==1?dy*fraction:5*std::sin(heading);
            if(!std::isfinite(get(controller+144)) || !std::isfinite(get(controller+148)) ||
               std::abs(get(controller+144)-expected_x)>5e-4 || std::abs(get(controller+148)-expected_y)>5e-4) {
                REXLOG_ERROR("Native character turn mismatch: {} Hz (0=uneven), mode {}, scaled {}, paused {}, angle {}, frame {}, elapsed {}, velocity {}/{}, expected {}/{}",
                    hz,mode,scaled,paused,angle,frame,effective,get(controller+144),get(controller+148),expected_x,expected_y);return false;
            }
        }
        ++turn_scenarios;
    }
    REXLOG_INFO("Native character turning passed {} original-code steps / {} scenarios: 90/135-degree direction changes, linear/angular smoothing at30/60/120/240Hz and uneven1/240..1/40 intervals, half-speed and pause; contacts/full character controller excluded",turn_steps,turn_scenarios);
    REXLOG_INFO("Native character movement helper passed {} original-code steps / {} scenarios at 30..240 Hz: straight acceleration/release, both smoothing modes, scaled/unscaled clocks and pause; collision/jump integration excluded",steps,scenarios);
    return true;
}
