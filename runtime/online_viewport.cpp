#include "online_viewport.h"
#include "online_runtime.h"
#include "online_room_gameplay.h"
#include "online_native_menu.h"
#include "online.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <cstring>
#include <optional>
#include <initializer_list>
#include <array>
#include <string>

PPC_EXTERN_FUNC(__imp__sub_829D9698);
PPC_EXTERN_FUNC(__imp__sub_829F3B40);
PPC_EXTERN_FUNC(__imp__sub_82851018);
PPC_EXTERN_FUNC(__imp__sub_829EFF08);
REXCVAR_DEFINE_BOOL(em2_online_single_view,true,"Online","Use the local player's full-window native camera and HUD during two-human online play");
REXCVAR_DEFINE_BOOL(em2_native_lobby_qa_input_sequence,false,"Debug","Private validated loopback only: fixed native controller movement, jump and camera sequence");
namespace {
thread_local uint32_t online_view_app=0;
struct Preserve {
    PPCContext& ctx;PPCContext saved=ctx;DWORD error=GetLastError();unsigned csr=_mm_getcsr();
    ~Preserve(){ctx=saved;SetLastError(error);_mm_setcsr(csr);}
};
bool Readable(uint32_t address,size_t size,uint8_t* base){
    return base && address>=4096 && uint64_t(address)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,size);
}
bool Enabled(bool enabled,std::optional<unsigned> slot){return enabled && slot && *slot<2;}
void Configure(PPCContext& ctx,uint8_t* base,bool enabled,std::optional<unsigned> slot,PPCFunc* original){
    // Original app routine owns viewport/frustum setup, Scaleform layout,
    // renderer split state and postprocess targets. Keep native actor/profile
    // counts alone: changing them would restore the single-player companion.
    const auto app=ctx.r3.u32;
    const bool active=Enabled(enabled,slot);
    if(active)ctx.r4.u64=0;
    original(ctx,base);
    if(active){
        Preserve preserve{ctx};em2::GuestReadBatch reads;
        online_view_app=app;
        if(Readable(app,0x2090C,base))REXLOG_INFO("Online single-view layout: local {}, split {}, app {:08X}",*slot,unsigned(PPC_LOAD_U8(app+0x107A4)),app);
    }
}
uint32_t GuestCamera(PPCContext& ctx,uint8_t* base,PPCFunc* resolve){
    Preserve preserve{ctx};em2::GuestReadBatch reads;
    if(!Readable(0x833E201C,4,base))return 0;
    const auto owned=em2::net::NativeRoomLocalCameraController();
    const auto controller=owned?owned:PPC_LOAD_U32(0x833E201C);
    if(!Readable(controller,28,base) || PPC_LOAD_U32(controller)!=0x82112778)return 0;
    // Original controller->entity registry->scene component->NiCamera lookup.
    // Do not swap controllers, poses, camera targets or simulation globals.
    ctx.r3.u64=controller;
    {em2::GuestReadBatch::CallbackBoundary boundary;resolve(ctx,base);}
    const auto camera=ctx.r3.u32;
    em2::GuestReadBatch fresh;
    return Readable(camera,368,base) && PPC_LOAD_U32(camera)==0x8203E610?camera:0;
}
void Construct(PPCContext& ctx,uint8_t* base,bool enabled,std::optional<unsigned> slot,PPCFunc* resolve,PPCFunc* original){
    const bool primary=Enabled(enabled,slot) && ctx.lr==0x829EC82C && ctx.r8.u64==0;
    const auto requested=ctx.r4.u32;
    const bool local=Enabled(enabled,slot) && (*slot==1 || em2::net::NativeRoomLocalCameraController()) && ctx.lr==0x829EC82C && ctx.r8.u64==0;
    if(local){
        if(const auto camera=GuestCamera(ctx,base,resolve))ctx.r4.u64=camera;
        else {Preserve preserve{ctx};REXLOG_WARN("Online single-view camera unavailable at native primary-view construction; acceptance incomplete");}
    }
    // Original constructor retains the selected camera through its normal
    // NiRefObject ownership, including destruction and replacement on load.
    const auto selected=ctx.r4.u32;
    original(ctx,base);
    if(primary){
        Preserve preserve{ctx};em2::GuestReadBatch reads;
        const auto view=ctx.r3.u32;
        const auto retained=Readable(view,52,base)?PPC_LOAD_U32(view+48):0;
        REXLOG_INFO("Online single-view primary: local {}, requested {:08X}, selected {:08X}, retained {:08X}, view {:08X}",*slot,requested,selected,retained,view);
    }
}
}
PPC_FUNC_IMPL(sub_829D9698){Configure(ctx,base,REXCVAR_GET(em2_online_single_view),em2::net::NativeOnlineDisplaySlot(),__imp__sub_829D9698);}
PPC_FUNC_IMPL(sub_829F3B40){Construct(ctx,base,REXCVAR_GET(em2_online_single_view),em2::net::NativeOnlineDisplaySlot(),__imp__sub_82851018,__imp__sub_829F3B40);}

unsigned em2::net::NativeOnlineHudMode(){
    if(!REXCVAR_GET(em2_online_single_view))return 0;
    const auto slot=NativeOnlineDisplaySlot();
    return slot && *slot<2?2+*slot:0;
}
bool em2::net::BindNativeRoomLocalView(PPCContext& ctx,uint8_t* base){
    const auto controller=NativeRoomLocalCameraController();if(!controller)return true;
    if(!REXCVAR_GET(em2_online_single_view) || !Readable(online_view_app,0x2090C,base) || PPC_LOAD_U8(online_view_app+0x107A4))return false;
    const auto view=PPC_LOAD_U32(online_view_app+0x208F4);
    if(!Readable(view,80,base) || PPC_LOAD_U32(view)!=0x82109F40)return false;
    Preserve preserve{ctx};
    const auto top=ctx.r1.u32&~15u;
    if(top<32768 || !Readable(top-16384,16384,base))return false;
    struct Stack {uint8_t* memory;std::array<uint8_t,16384> bytes;explicit Stack(uint8_t* p):memory(p){std::memcpy(bytes.data(),p,bytes.size());}~Stack(){std::memcpy(memory,bytes.data(),bytes.size());}}stack(base+top-16384);
    ctx.r1.u64=top-4096;
    const auto camera=GuestCamera(ctx,base,__imp__sub_82851018);if(!camera)return false;
    const auto previous=PPC_LOAD_U32(view+48);if(previous==camera)return true;
    // Same NiRefObject ownership as the view constructor/destructor: retain
    // the new output and release the old output through native NiPointer.
    ctx.r3.u64=view+48;ctx.r4.u64=camera;__imp__sub_829EFF08(ctx,base);
    const bool retained=PPC_LOAD_U32(view+48)==camera;
    REXLOG_INFO("Native Room local view rebound: controller {:08X} view {:08X} previous {:08X} retained {:08X} matches {} split {}; native NiPointer ownership",controller,view,previous,PPC_LOAD_U32(view+48),retained,unsigned(PPC_LOAD_U8(online_view_app+0x107A4)));
    return retained;
}

uint32_t em2::net::NativeRoomViewCamera(uint8_t* base){
    if(!CurrentNativeRoomGameplay() || !Readable(online_view_app,0x2090C,base) || PPC_LOAD_U8(online_view_app+0x107A4))return 0;
    const auto view=PPC_LOAD_U32(online_view_app+0x208F4);
    if(!Readable(view,80,base) || PPC_LOAD_U32(view)!=0x82109F40)return 0;
    const auto camera=PPC_LOAD_U32(view+48);
    return Readable(camera,368,base) && PPC_LOAD_U32(camera)==0x8203E610?camera:0;
}

namespace {
struct InputPhase {uint64_t begin,end;unsigned slot;const char* name;em2::net::Pad pad;};
// Absolute source ticks, not render frames or wall time. Neutral periods keep
// input edges deliberate and leave the native co-op join its normal inputs.
constexpr std::array<InputPhase,8> input_phases{{
    {120,132,0,"Mickey forward",{0,0,0,0,24000}},
    {180,192,1,"Oswald forward",{0,0,0,0,24000}},
    {240,242,0,"Mickey jump",{0x1000}},
    {270,272,1,"Oswald jump",{0x1000}},
    {330,360,0,"Mickey camera right",{0,0,0,0,0,20000}},
    {390,420,1,"Oswald camera right",{0,0,0,0,0,20000}},
    {450,462,0,"Mickey forward after camera",{0,0,0,0,24000}},
    {510,522,1,"Oswald forward after camera",{0,0,0,0,24000}}
}};
}
void em2::net::ApplyPrivateOnlineInputSequence(uint64_t tick,unsigned slot,bool joining,Pad& pad){
    if(!REXCVAR_GET(em2_native_lobby_qa_input_sequence) || !NativeOnlineMenuQaActive())return;
    const auto local=NativeOnlineDisplaySlot();if(!local || *local!=slot || slot>=2)return;
    const DWORD error=GetLastError();const unsigned csr=_mm_getcsr();
    struct Restore{DWORD error;unsigned csr;~Restore(){SetLastError(error);_mm_setcsr(csr);}}restore{error,csr};
    pad={};
    static thread_local std::optional<uint64_t> last_log;
    const bool log=!last_log || *last_log!=tick;
    for(const auto& phase:input_phases)if(slot==phase.slot && tick>=phase.begin && tick<phase.end){
        if(!joining)pad=phase.pad;
        if(log && tick==phase.begin)REXLOG_INFO("Native online input sequence: local {}, source tick {}, phase '{}', joining {}",slot,tick,phase.name,joining);
    }
    if(log && (tick==100 || tick==600))REXLOG_INFO("Native online input sequence: local {}, source tick {}, phase '{}', joining {}",slot,tick,tick==100?"neutral baseline":"complete",joining);
    last_log=tick;
}

void em2::net::ObservePrivateOnlineView(PPCContext& ctx,uint8_t* base,uint64_t tick){
    if(!REXCVAR_GET(em2_native_lobby_qa_input_sequence) || !REXCVAR_GET(em2_online_single_view) ||
       tick<95 || tick>620 || !NativeOnlineMenuQaActive())return;
    const auto local=NativeOnlineDisplaySlot();if(!local || *local>=2)return;
    Preserve preserve{ctx};em2::GuestReadBatch reads;
    if(!Readable(online_view_app,0x208F8,base))return;
    const auto view=PPC_LOAD_U32(online_view_app+0x208F4);
    if(!Readable(view,52,base) || PPC_LOAD_U32(view)!=0x82109F40)return;
    const auto camera=PPC_LOAD_U32(view+48);
    if(!Readable(camera,368,base) || PPC_LOAD_U32(camera)!=0x8203E610)return;
    auto words=[&](std::initializer_list<unsigned> offsets){
        constexpr char digits[]="0123456789ABCDEF";std::string result;
        for(auto offset:offsets){if(!result.empty())result+='/';const auto word=PPC_LOAD_U32(camera+offset);
            for(int shift=28;shift>=0;shift-=4)result+=digits[(word>>shift)&15];}
        return result;
    };
    REXLOG_INFO("Online local view pose: local {}, tick {}, camera {:08X}, rotation {}, position {}, viewport {}",*local,tick,camera,
        words({144,148,152,160,164,168,176,180,184}),words({192,196,200}),words({340,344,348,352}));
}

bool em2::net::TestNativeOnlineViewport(){
    const bool input_setting=REXCVAR_GET(em2_native_lobby_qa_input_sequence);
    struct InputSetting{bool value;~InputSetting(){REXCVAR_SET(em2_native_lobby_qa_input_sequence,value);}}input_restore{input_setting};
    REXCVAR_SET(em2_native_lobby_qa_input_sequence,true);
    const Pad physical{0x2345,10,20,-1000,2000,-3000,4000,true};
    for(auto tick:{0ull,120ull,330ull,600ull,UINT64_MAX}){
        auto sampled=physical;ApplyPrivateOnlineInputSequence(tick,0,false,sampled);
        if(sampled!=physical){REXLOG_ERROR("Private online input sequence changed non-QA input");return false;}
    }
    REXCVAR_SET(em2_native_lobby_qa_input_sequence,input_setting);
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Release{uint8_t* base;DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Release(){VirtualFree(base,0,MEM_RELEASE);SetLastError(error);_mm_setcsr(csr);}}release{base};
    if(!VirtualAlloc(base,65536,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x833E0000,65536,MEM_COMMIT,PAGE_READWRITE))return false;
    PPC_STORE_U32(0x833E201C,0x2000);PPC_STORE_U32(0x2000,0x82112778);PPC_STORE_U32(0x2014,0x3000);PPC_STORE_U32(0x3000,0x8203E610);
    const auto resolve=+[](PPCContext& c,uint8_t* base){c.r3.u64=PPC_LOAD_U32(c.r3.u32+20);c.r4.u64=0xBAD;c.f1.f64=72;SetLastError(0xBAD);};
    const auto original=+[](PPCContext& c,uint8_t* base){PPC_STORE_U32(0x4000,c.r4.u32);c.r3.u64=0x9876;};
    unsigned checks=0;
    for(bool enabled:{false,true})for(auto slot:{std::optional<unsigned>{},std::optional<unsigned>{0},std::optional<unsigned>{1},std::optional<unsigned>{2}})
    for(bool exact:{false,true})for(unsigned native_slot:{0u,1u}){
        PPCContext input{};input.r1.u64=0xF000;input.r3.u64=0x1000;input.r4.u64=0x1111;input.r8.u64=native_slot;input.lr=exact?0x829EC82C:0x829EC914;input.fpscr.InitHost();
        auto expected=input;if(Enabled(enabled,slot) && *slot==1 && exact && !native_slot)expected.r4.u64=0x3000;
        auto actual=input;SetLastError(0xA504);Construct(actual,base,enabled,slot,resolve,original);original(expected,base);
        if(std::memcmp(&actual,&expected,sizeof(actual)) || GetLastError()!=0xA504)return false;
        ++checks;
        expected=input;if(Enabled(enabled,slot))expected.r4.u64=0;actual=input;Configure(actual,base,enabled,slot,original);original(expected,base);
        if(std::memcmp(&actual,&expected,sizeof(actual)))return false;
        ++checks;
    }
    REXLOG_INFO("Online viewport adapter passed {} gateway/caller/local-slot and full-context checks; original renderer/camera lookup and live HUD acceptance remain separate",checks);
    return true;
}
