#include "camera_look.h"
#include "controls.h"
#include "online_room_gameplay.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <immintrin.h>
#include <stdexcept>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_827C5990);
PPC_EXTERN_FUNC(__imp__sub_82A5D278);
PPC_EXTERN_FUNC(__imp__sub_82769490);
PPC_EXTERN_FUNC(sub_827C5990);
PPC_EXTERN_FUNC(__imp__sub_827C56F8);
PPC_EXTERN_FUNC(__imp__sub_830ACF88);
PPC_EXTERN_FUNC(__imp__sub_830A7C10);
PPC_EXTERN_FUNC(__imp__sub_830A7C70);
PPC_EXTERN_FUNC(__imp__sub_827C3D78);
PPC_EXTERN_FUNC(__imp__sub_8276A1F8);
PPC_EXTERN_FUNC(__imp__sub_822F1C10);
REXCVAR_DECLARE(bool,em2_present_camera);
namespace {
using Binding=em2::NativeCameraLookBinding;
bool Readable(uint32_t address,size_t size,uint8_t* base){return address>=4096 && uint64_t(address)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,size);}
float ReadFloat(uint32_t address,uint8_t* base){return std::bit_cast<float>(PPC_LOAD_U32(address));}
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
struct ContextScope {PPCContext& ctx;PPCContext saved;Environment environment;explicit ContextScope(PPCContext& context):ctx(context),saved(context){}~ContextScope(){ctx=saved;}};
struct Observation {uint32_t manager=0,action=0;bool enabled=false;uint64_t sequence=0;};
thread_local std::array<Observation,16> observations{};
thread_local std::array<Binding,32> bindings{};
thread_local std::array<Binding,16> owned_bindings{};
thread_local uint64_t sequence=0;
thread_local bool test_observation=false;
struct AxisQuery {uint32_t manager=0,user=0;std::array<float,2> axes{};unsigned hits=0;};
thread_local AxisQuery* query=nullptr;
thread_local const Binding* owned_dispatch=nullptr;
class QueryScope {AxisQuery* saved=query;public:explicit QueryScope(AxisQuery& next){query=&next;}~QueryScope(){query=saved;}};
bool Fingerprint(Binding& result,uint8_t* base){
    em2::GuestReadBatch reads;
    const auto manager=result.manager,action=result.action;
    if(!Readable(manager,76,base) || !Readable(action,436,base) || PPC_LOAD_U32(action+80)!=4)return false;
    if(PPC_LOAD_U32(action+408)!=2)return false;
    const auto pad=action+88;
    const auto user=PPC_LOAD_U32(pad),input=PPC_LOAD_U32(manager+72);
    if(user>1 || !Readable(input,88,base) || PPC_LOAD_U32(pad+32)!=user ||
        PPC_LOAD_U32(pad+4)!=0x02400019 || PPC_LOAD_U32(pad+36)!=0x02400020 ||
        PPC_LOAD_U32(pad+8)!=0 || PPC_LOAD_U32(pad+40)!=3 ||
        PPC_LOAD_U8(pad+16)>1 || PPC_LOAD_U8(pad+48)>1 || PPC_LOAD_U32(action+432)>2)return false;
    const auto device=PPC_LOAD_U32(input+72+user*4);
    if(!Readable(device,72,base) || PPC_LOAD_U32(device+56)<4)return false;
    const auto history=PPC_LOAD_U32(device+60);
    if(!Readable(history,128,base))return false;
    for(unsigned axis:{2u,3u}){
        const auto record=history+axis*32;
        if(int32_t(PPC_LOAD_U32(record+16))!=-32768 || PPC_LOAD_U32(record+20)!=32767 ||
            int32_t(PPC_LOAD_U32(record+24))!=-100 || PPC_LOAD_U32(record+28)!=100)return false;
    }
    const auto deadzone=ReadFloat(action+416,base),saturation=ReadFloat(action+420,base),power=ReadFloat(action+424,base),gain=ReadFloat(action+428,base);
    if(!std::isfinite(deadzone) || !std::isfinite(saturation) || !std::isfinite(power) || !std::isfinite(gain) ||
        deadzone<0 || saturation<=deadzone || saturation>4 || power<=0 || power>16 || gain<0 || gain>8)return false;
    result.input_system=input;result.device=device;result.user=user;
    unsigned n=0;auto add=[&](uint32_t value){result.fingerprint[n++]=value;};
    add(PPC_LOAD_U32(manager));add(PPC_LOAD_U32(input));add(PPC_LOAD_U32(device));add(PPC_LOAD_U32(device+56));add(PPC_LOAD_U32(device+68));
    add(PPC_LOAD_U32(action));add(PPC_LOAD_U32(action+80));add(PPC_LOAD_U32(action+408));
    for(unsigned offset=0;offset<64;offset+=4)add(PPC_LOAD_U32(pad+offset));
    for(unsigned offset=416;offset<=432;offset+=4)add(PPC_LOAD_U32(action+offset));
    for(unsigned axis:{2u,3u})for(unsigned offset=16;offset<=28;offset+=4)add(PPC_LOAD_U32(history+axis*32+offset));
    return n==result.fingerprint.size();
}
void Invalidate(uint32_t action){for(auto& binding:bindings)if(binding.action==action)binding={};}
void ObserveAction(uint32_t manager,uint32_t action,bool enabled,uint8_t* base){
    auto found=std::find_if(observations.begin(),observations.end(),[&](const Observation& value){return value.action==action;});
    Binding checked;checked.manager=manager;checked.action=action;
    const bool eligible=enabled && Fingerprint(checked,base);
    if(!eligible){
        // Input dispatch evaluates many unrelated actions between camera look
        // callbacks. They must not evict a valid captured camera action.
        if(found!=observations.end())found->enabled=false;
        Invalidate(action);return;
    }
    if(found==observations.end())found=std::min_element(observations.begin(),observations.end(),[](const auto& a,const auto& b){return a.sequence<b.sequence;});
    *found={manager,action,true,++sequence};
}
bool Validate(const Binding& binding,uint8_t* base){
    if(!binding.registry || !Readable(binding.mode,24,base) || !Readable(binding.controller,28,base) ||
        PPC_LOAD_U32(binding.mode)!=0x82126C98 || PPC_LOAD_U32(binding.mode+20)!=binding.controller ||
        PPC_LOAD_U32(binding.controller)!=0x82112778 || PPC_LOAD_U32(binding.controller+24)!=binding.mode ||
        PPC_LOAD_U32(0x833DA7E4)!=binding.registry)return false;
    const auto observed=std::find_if(observations.begin(),observations.end(),[&](const Observation& value){return value.manager==binding.manager && value.action==binding.action;});
    if(observed==observations.end() || !observed->enabled)return false;
    auto current=binding;
    return Fingerprint(current,base) && current==binding;
}
bool ValidateSelectedInput(const Binding& binding,uint8_t* base){
    em2::GuestReadBatch reads;
    const auto manager=PPC_LOAD_U32(0x833DAB60);
    // Secondary controllers remap action UIDs inside 82A5D278. That mapping is
    // not part of this primary-camera replay and must remain native.
    if(!Readable(binding.controller,20,base) || PPC_LOAD_U32(binding.controller+16)!=0 || !Readable(manager,712,base))return false;
    PPCContext selected{};selected.r3.u64=manager;selected.r4.u64=0;
    __imp__sub_82769490(selected,base);
    return Readable(selected.r3.u32,4,base) && PPC_LOAD_U32(selected.r3.u32)==PPC_LOAD_U32(binding.action);
}
struct DiscoveryScratch {
    ContextScope preserve;uint8_t* base;uint32_t stack=0,low=0;std::array<uint8_t,8192> saved{};
    DiscoveryScratch(PPCContext& ctx,uint8_t* memory):preserve(ctx),base(memory){const auto top=ctx.r1.u32&~15u;
        if(top>=12288 && Readable(top-8192,saved.size(),base)){low=top-8192;stack=top-4096;std::memcpy(saved.data(),base+low,saved.size());ctx.r1.u64=stack;}}
    ~DiscoveryScratch(){if(stack)std::memcpy(base+low,saved.data(),saved.size());}
};
bool ValidateOwnedSelectedInput(const Binding& binding,PPCContext& ctx,uint8_t* base){
    if(!em2::net::NativeRoomOwnsCameraMode(binding.mode,base) || binding.user>1 ||
        !Readable(binding.controller,20,base) || PPC_LOAD_U32(binding.controller+16)!=binding.user)return false;
    const auto manager=PPC_LOAD_U32(0x833DAB60);if(!Readable(manager,712,base))return false;
    DiscoveryScratch scratch(ctx,base);if(!scratch.stack)return false;
    // Native sentinel selects the analog rotation family without mutating
    // either retail player's input scheme or indexing a room peer as a player.
    ctx.r3.u64=manager;ctx.r4.s64=-1;__imp__sub_82769490(ctx,base);
    if(!Readable(ctx.r3.u32,4,base))return false;const auto selected=PPC_LOAD_U32(ctx.r3.u32);
    if(!binding.user)return selected==PPC_LOAD_U32(binding.action);
    // Original82A5D278 maps the secondary action name before comparing it
    // with the selected Look family. Use that exact native map, not suffix
    // stripping or comparing Look2 with the primary Look UID.
    const auto name=scratch.stack+512;PPC_STORE_U32(name,0);
    ctx.r3.u64=name;ctx.r4.u64=manager;ctx.r5.u64=binding.action;__imp__sub_8276A1F8(ctx,base);
    const auto mapped=PPC_LOAD_U32(name);
    ctx.r3.u64=name;__imp__sub_822F1C10(ctx,base);
    return mapped && selected==mapped;
}
bool OwnedContext(const Binding& binding,PPCContext& ctx,uint8_t* base){
    if(!Readable(0x833DBAD8,4,base) || PPC_LOAD_U32(0x833DBAD8)!=binding.manager ||
        !Readable(binding.manager,76,base) || !Readable(binding.event,24,base))return false;
    DiscoveryScratch scratch(ctx,base);if(!scratch.stack)return false;
    ctx.r3.u64=binding.manager;ctx.r4.u64=binding.manager+16;__imp__sub_827C3D78(ctx,base);
    if(binding.event!=binding.manager+24 && binding.event!=ctx.r3.u32)return false;
    const auto actions=PPC_LOAD_U32(binding.event+16),count=PPC_LOAD_U32(binding.event+20);
    if(count>256 || (count && !Readable(actions,size_t(count)*4,base)))return false;
    unsigned found=0;for(unsigned i=0;i<count;++i)found+=PPC_LOAD_U32(actions+i*4)==binding.action;
    return found==1;
}
bool ValidateOwned(const Binding& binding,PPCContext& ctx,uint8_t* base){
    if(!binding.registry || !Readable(binding.mode,24,base) || !Readable(binding.controller,28,base) ||
        PPC_LOAD_U32(binding.mode)!=0x82126C98 || PPC_LOAD_U32(binding.mode+20)!=binding.controller ||
        PPC_LOAD_U32(binding.controller)!=0x82112778 || PPC_LOAD_U32(binding.controller+24)!=binding.mode ||
        PPC_LOAD_U32(0x833DA7E4)!=binding.registry)return false;
    auto current=binding;
    return Fingerprint(current,base) && current==binding && ValidateOwnedSelectedInput(binding,ctx,base) && OwnedContext(binding,ctx,base);
}
bool AxisOverride(PPCContext& ctx){
    if(!query || ctx.lr!=0x827C5A0C || ctx.r3.u32!=query->manager || ctx.r5.u32!=query->user || ctx.r6.u32!=0)return false;
    const unsigned axis=ctx.r4.u32==0x02400019?0:ctx.r4.u32==0x02400020?1:2;
    if(axis==2)return false;
    ctx.fpscr.disableFlushMode();ctx.f1.f64=double(query->axes[axis]);++query->hits;return true;
}
bool RunMath(const Binding& binding,const std::array<float,2>& axes,PPCContext& ctx,uint8_t* base,em2::NativeCameraLookResult& output,PPCFunc* action_math=__imp__sub_827C5990){
    ContextScope restore(ctx);
    if(query)return false;
    const auto top=ctx.r1.u32 & ~15u;
    if(top<12288 || !Readable(top-8192,8192,base))return false;
    // Private scratch BELOW the current render stack; never restore an old
    // simulation context, old stack pointer or old TLS. Restore scratch too.
    std::array<uint8_t,8192> saved;
    std::memcpy(saved.data(),base+top-8192,saved.size());
    struct RestoreStack {uint8_t* destination;const std::array<uint8_t,8192>& saved;~RestoreStack(){std::memcpy(destination,saved.data(),saved.size());}}restore_stack{base+top-8192,saved};
    const auto stack=top-4096,x=stack+512,y=stack+516;
    PPC_STORE_U32(x,0);PPC_STORE_U32(y,0);
    AxisQuery injected{binding.manager,binding.user,axes,0};QueryScope scoped(injected);
    ctx.r1.u64=stack;ctx.r3.u64=binding.manager;ctx.r4.u64=binding.action;ctx.r5.u64=1;ctx.r6.u64=x;ctx.r7.u64=y;
    action_math(ctx,base);
    const float result_x=ReadFloat(x,base),result_y=ReadFloat(y,base);
    if(ctx.r3.u32!=1 || injected.hits!=2 || !std::isfinite(result_x) || !std::isfinite(result_y))return false;
    output={result_x,result_y,binding.event,binding.action};return true;
}
}
void em2::ResetNativeCameraLookBindings(){observations={};bindings={};owned_bindings={};sequence=0;}
void em2::ObserveNativeCameraLookBinding(uint32_t mode,uint32_t event,uint32_t action,uint8_t* base) noexcept {
    Environment preserve;
    if(!test_observation && !REXCVAR_GET(em2_present_camera))return;
    try{

        const auto found=std::find_if(observations.begin(),observations.end(),[&](const Observation& value){return value.action==action && value.enabled;});
        if(found==observations.end())return; // Other digital actions share OnLook.
        Binding binding;binding.mode=mode;binding.event=event;binding.action=action;binding.manager=found->manager;
        binding.registry=PPC_LOAD_U32(0x833DA7E4);
        if(!Readable(mode,24,base))return;
        binding.controller=PPC_LOAD_U32(mode+20);
        if(!Fingerprint(binding,base) || !Validate(binding,base))return;
        auto slot=std::find_if(bindings.begin(),bindings.end(),[&](const Binding& value){return value.mode==mode && value.user==binding.user && value.action==action;});
        if(slot==bindings.end())slot=std::find_if(bindings.begin(),bindings.end(),[](const Binding& value){return !value.mode;});
        if(slot!=bindings.end())*slot=binding;
    }catch(...){}
}
bool em2::NativeCameraLookBindingFor(uint32_t mode,uint8_t* base,Binding& output,unsigned user){
    Environment preserve;output={};
    for(const auto& binding:bindings){
        if(binding.mode==mode && binding.user==user && Validate(binding,base) && ValidateSelectedInput(binding,base)){
            output=binding;return true;
        }
    }
    return false;
}
bool em2::ReplayNativeCameraLookAction(const Binding& binding,const LiveLookInput& input,PPCContext& ctx,uint8_t* base,NativeCameraLookResult& output){
    Environment preserve;output={};
    if(!Validate(binding,base) || !ValidateSelectedInput(binding,base))return false;
    std::array<float,2> axes;
    if(!ConvertNativeDeviceLook(input,ReadFloat(0x833B65FC,base),axes))return false;
    return RunMath(binding,axes,ctx,base,output);
}
bool em2::NativeOwnedCameraLookBindingFor(uint32_t mode,PPCContext& ctx,uint8_t* base,Binding& output,unsigned user){
    Environment preserve;output={};
    for(const auto& binding:owned_bindings)if(binding.mode==mode && binding.user==user && ValidateOwned(binding,ctx,base)){output=binding;return true;}
    if(user>1 || !em2::net::NativeRoomOwnsCameraMode(mode,base) || !Readable(mode,24,base))return false;
    const auto manager=PPC_LOAD_U32(0x833DBAD8);if(!Readable(manager,76,base))return false;
    DiscoveryScratch scratch(ctx,base);if(!scratch.stack)return false;
    ctx.r3.u64=manager;ctx.r4.u64=manager+16;__imp__sub_827C3D78(ctx,base);const auto active=ctx.r3.u32;
    Binding selected;unsigned matches=0;
    const std::array<uint32_t,2> events{manager+24,active};
    for(unsigned e=0;e<events.size();++e){const auto event=events[e];if(e && event==events[0])continue;
        if(!Readable(event,24,base))return false;const auto list=PPC_LOAD_U32(event+16),count=PPC_LOAD_U32(event+20);
        if(count>256 || (count && !Readable(list,size_t(count)*4,base)))return false;
        for(unsigned i=0;i<count;++i){Binding candidate;candidate.mode=mode;candidate.controller=PPC_LOAD_U32(mode+20);candidate.registry=PPC_LOAD_U32(0x833DA7E4);
            candidate.manager=manager;candidate.event=event;candidate.action=PPC_LOAD_U32(list+i*4);
            if(!Fingerprint(candidate,base) || candidate.user!=user || !ValidateOwned(candidate,ctx,base))continue;
            selected=candidate;++matches;
        }
    }
    if(matches!=1)return false;
    auto slot=std::find_if(owned_bindings.begin(),owned_bindings.end(),[&](const auto& value){return value.mode==mode;});
    if(slot==owned_bindings.end())slot=std::find_if(owned_bindings.begin(),owned_bindings.end(),[&](const auto& value){return !value.mode || !em2::net::NativeRoomOwnsCameraMode(value.mode,base);});
    if(slot==owned_bindings.end())return false;*slot=selected;output=selected;return true;
}
bool em2::ReplayNativeOwnedCameraLookAction(const Binding& binding,const LiveLookInput& input,PPCContext& ctx,uint8_t* base,NativeCameraLookResult& output){
    Environment preserve;output={};if(!ValidateOwned(binding,ctx,base))return false;
    std::array<float,2> axes;if(!ConvertNativeDeviceLook(input,ReadFloat(0x833B65FC,base),axes))return false;
    return RunMath(binding,axes,ctx,base,output);
}
bool em2::DispatchNativeOwnedCameraLook(const Binding& binding,const NativeCameraLookResult& result,PPCContext& ctx,uint8_t* base){
    if(owned_dispatch || result.event!=binding.event || result.action!=binding.action ||
        !std::isfinite(result.x) || !std::isfinite(result.y) || !ValidateOwned(binding,ctx,base))return false;
    ContextScope preserve(ctx);
    struct Permit {explicit Permit(const Binding& b){owned_dispatch=&b;}~Permit(){owned_dispatch=nullptr;}}permit(binding);
    ctx.r3.u64=binding.mode;ctx.r4.u64=result.event;ctx.r5.u64=result.action;ctx.f1.f64=result.x;ctx.f2.f64=result.y;
    __imp__sub_82A5D278(ctx,base);
    return true;
}
// Only the base normal-mode OnLook reached by the authoritative owned-camera
// dispatch uses the native analog selector. Ordinary retail dispatch, other
// modes and all calls outside this scope retain the original selection.
PPC_FUNC_IMPL(sub_82769490){
    if(owned_dispatch && ctx.lr==0x8299E844 && ctx.r31.u32==owned_dispatch->mode &&
        ctx.r3.u32==PPC_LOAD_U32(0x833DAB60) && ctx.r4.u32==owned_dispatch->user &&
        em2::net::NativeRoomOwnsCameraMode(owned_dispatch->mode,base)){
        const auto profile=ctx.r4;ctx.r4.s64=-1;__imp__sub_82769490(ctx,base);ctx.r4=profile;return;
    }
    __imp__sub_82769490(ctx,base);
}
void em2::TraceNativeOwnedLookDefinitions(uint32_t mode,PPCContext& ctx,uint8_t* base,unsigned user){
    Environment preserve;DiscoveryScratch scratch(ctx,base);if(!scratch.stack || user>1 || !net::NativeRoomOwnsCameraMode(mode,base))return;
    const auto manager=PPC_LOAD_U32(0x833DBAD8),players=PPC_LOAD_U32(0x833DAB60);
    if(!Readable(manager,76,base) || !Readable(players,712,base))return;
    ctx.r3.u64=players;ctx.r4.s64=-1;__imp__sub_82769490(ctx,base);const auto selected=Readable(ctx.r3.u32,4,base)?PPC_LOAD_U32(ctx.r3.u32):0;
    ctx.r3.u64=manager;ctx.r4.u64=manager+16;__imp__sub_827C3D78(ctx,base);const auto active=ctx.r3.u32;
    REXLOG_WARN("Owned look definition scan: mode {:08X} user {} manager {:08X} active {:08X} selected UID {:08X}",mode,user,manager,active,selected);
    for(auto event:{manager+24,active}){if(!Readable(event,24,base))continue;const auto list=PPC_LOAD_U32(event+16),count=PPC_LOAD_U32(event+20);
        if(count>256 || (count && !Readable(list,size_t(count)*4,base)))continue;
        for(unsigned i=0;i<count;++i){const auto action=PPC_LOAD_U32(list+i*4);if(!Readable(action,436,base) || PPC_LOAD_U32(action+80)!=4)continue;
            Binding candidate;candidate.mode=mode;candidate.controller=PPC_LOAD_U32(mode+20);candidate.manager=manager;candidate.event=event;candidate.action=action;candidate.registry=PPC_LOAD_U32(0x833DA7E4);
            const bool shape=Fingerprint(candidate,base);
            const auto name=scratch.stack+512;PPC_STORE_U32(name,0);ctx.r3.u64=name;ctx.r4.u64=players;ctx.r5.u64=action;__imp__sub_8276A1F8(ctx,base);const auto mapped=PPC_LOAD_U32(name);ctx.r3.u64=name;__imp__sub_822F1C10(ctx,base);
            REXLOG_WARN("Owned analog definition: event {:08X} action {:08X} UID {:08X} mapped {:08X} count {} binding {}/{:08X}/{}/{} and {}/{:08X}/{}/{} shape {} selected {}",event,action,PPC_LOAD_U32(action),mapped,PPC_LOAD_U32(action+408),
                PPC_LOAD_U32(action+88),PPC_LOAD_U32(action+92),PPC_LOAD_U32(action+96),unsigned(PPC_LOAD_U8(action+104)),PPC_LOAD_U32(action+120),PPC_LOAD_U32(action+124),PPC_LOAD_U32(action+128),unsigned(PPC_LOAD_U8(action+136)),shape,mapped==selected);
            if(PPC_LOAD_U32(action+408)==4)REXLOG_WARN("Owned analog remaining bindings: action {:08X} {}/{:08X}/{}/{} and {}/{:08X}/{}/{}",action,PPC_LOAD_U32(action+152),PPC_LOAD_U32(action+156),PPC_LOAD_U32(action+160),unsigned(PPC_LOAD_U8(action+168)),PPC_LOAD_U32(action+184),PPC_LOAD_U32(action+188),PPC_LOAD_U32(action+192),unsigned(PPC_LOAD_U8(action+200)));
        }
    }
}
PPC_FUNC_IMPL(sub_827C5990){
    if(!query && (test_observation || REXCVAR_GET(em2_present_camera))){
        Environment preserve;
        try{ObserveAction(ctx.r3.u32,ctx.r4.u32,ctx.r5.u8!=0,base);}catch(...){}
    }
    __imp__sub_827C5990(ctx,base);
}
PPC_FUNC_IMPL(sub_827C56F8){if(!AxisOverride(ctx))__imp__sub_827C56F8(ctx,base);}

namespace {
void ThrowAfterNativeAction(PPCContext& ctx,uint8_t* base){__imp__sub_827C5990(ctx,base);throw std::runtime_error("native action fixture unwind");}
}
bool em2::TestNativeCameraLookAction(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Restore {
        uint8_t* base;decltype(observations) actions=observations;decltype(bindings) modes=bindings;
        uint64_t serial=sequence;bool observing=test_observation;AxisQuery* pending=query;Environment environment;
        ~Restore(){observations=actions;bindings=modes;sequence=serial;test_observation=observing;query=pending;if(base)VirtualFree(base,0,MEM_RELEASE);}
    }restore{base};
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"private/image.bin";
    std::error_code error;const auto image_size=std::filesystem::file_size(path,error);
    if(error || !base || !VirtualAlloc(base,0x40000,MEM_COMMIT,PAGE_READWRITE) ||
        !VirtualAlloc(base+PPC_IMAGE_BASE,image_size,MEM_COMMIT,PAGE_READWRITE))return false;
    std::ifstream image(path,std::ios::binary);
    if(!image.read(reinterpret_cast<char*>(base+PPC_IMAGE_BASE),image_size))return false;
    constexpr uint32_t manager=0x2000,action=0x3000,controller=0x4000,mode=0x5000,input_system=0x6000,device=0x7000,history=0x8000,sdk=0x9000;
    auto put=[&](uint32_t address,float value){PPC_STORE_U32(address,std::bit_cast<uint32_t>(value));};
    auto context=[] {PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0x3F000;ctx.r13.u64=0x12345678;ctx.lr=0x87654321;return ctx;};
    auto setup=[&](unsigned user,unsigned curve,float deadzone,float saturation,float power,float gain,unsigned inverse){
        std::memset(base,0,0x40000);ResetNativeCameraLookBindings();test_observation=true;query=nullptr;
        PPC_STORE_U32(0x833DA7E4,0xA000);PPC_STORE_U32(0x833DAB60,0xA800);
        PPC_STORE_U32(0xA800+704,0xC001CAFE);PPC_STORE_U32(0xA800+708,0xC001CAFE);
        PPC_STORE_U32(manager,0x12345678);PPC_STORE_U32(manager+72,input_system);
        PPC_STORE_U32(input_system,0x12348765);PPC_STORE_U32(input_system+72+user*4,device);
        PPC_STORE_U32(device,0x56781234);PPC_STORE_U32(device+56,4);PPC_STORE_U32(device+60,history);
        PPC_STORE_U32(controller,0x82112778);PPC_STORE_U32(controller+24,mode);
        PPC_STORE_U32(mode,0x82126C98);PPC_STORE_U32(mode+20,controller);
        PPC_STORE_U32(action,0xC001CAFE);PPC_STORE_U32(action+80,4);PPC_STORE_U32(action+408,2);
        for(unsigned axis=0;axis<2;++axis){
            const auto binding=action+88+axis*32;
            PPC_STORE_U32(binding,user);PPC_STORE_U32(binding+4,axis?0x02400020:0x02400019);
            PPC_STORE_U32(binding+8,axis?3:0);PPC_STORE_U8(binding+16,uint8_t((inverse>>axis)&1));
        }
        put(action+416,deadzone);put(action+420,saturation);put(action+424,power);put(action+428,gain);PPC_STORE_U32(action+432,curve);
        for(unsigned axis=0;axis<4;++axis){const auto record=history+axis*32;
            PPC_STORE_U32(record+16,uint32_t(-32768));PPC_STORE_U32(record+20,32767);PPC_STORE_U32(record+24,uint32_t(-100));PPC_STORE_U32(record+28,100);}
        ObserveAction(manager,action,true,base);ObserveNativeCameraLookBinding(mode,0x24681357,action,base);
    };
    unsigned checks=0,cases=0;bool ok=true;
    auto check=[&](bool condition,const char* message){++checks;if(!condition)REXLOG_ERROR("Native camera look action: {}",message);ok&=condition;return condition;};
    const std::array<LiveLookInput,18> samples={LiveLookInput{0,0},{1,-1},{-1,1},{32767,0},{-32768,0},{0,32767},{0,-32768},
        {32767,32767},{-32768,-32768},{32767,-32768},{-32768,32767},{8688,-8688},{8689,-8689},{8690,-8690},{10000,15000},{20000,-20000},{-30000,5000},{16384,16384}};
    std::array<uint8_t,0x40000> memory;
    for(unsigned user:{0u,1u})for(unsigned curve:{0u,1u,2u})for(unsigned inverse:{0u,1u,2u,3u})for(unsigned tuning=0;tuning<3;++tuning){
        setup(user,curve,tuning==0?.2f:tuning==1?0.f:.35f,tuning==2?.8f:.95f,tuning==1?1.f:tuning==2?2.3f:1.6f,tuning==2?1.25f:.5f,inverse);
        Binding binding;if(!check(NativeCameraLookBindingFor(mode,base,binding,user),"actual observed right-stick action associates with exact normal mode"))return false;
        for(const auto& sample:samples){
            put(0x833B65FC,tuning==1?0.f:8689.f/32768.f);
            PPC_STORE_U16(sdk+12,uint16_t(sample.x));PPC_STORE_U16(sdk+14,uint16_t(sample.y));
            auto producer=context();producer.r3.u64=device;producer.r4.u64=sdk;__imp__sub_830ACF88(producer,base);
            std::array<float,2> oracle;
            for(unsigned axis=0;axis<2;++axis){auto native=context();native.r3.u64=device;native.r4.u64=axis+2;__imp__sub_830A7C10(native,base);
                native.r5.u64=native.r3.u64;native.r3.u64=device;native.r4.u64=axis+2;__imp__sub_830A7C70(native,base);oracle[axis]=float(double(float(native.r3.s32))*double(.01f));}
            auto ctx=context();const auto saved=ctx;const auto csr=_mm_getcsr();const auto last_error=GetLastError();
            std::memcpy(memory.data(),base,memory.size());
            NativeCameraLookResult expected,actual;
            if(!check(RunMath(binding,oracle,ctx,base,expected) && ReplayNativeCameraLookAction(binding,sample,ctx,base,actual),"real native radial/per-axis/binding/response pipeline returns two finite axes"))return false;
            if(!check(std::bit_cast<uint32_t>(expected.x)==std::bit_cast<uint32_t>(actual.x) && std::bit_cast<uint32_t>(expected.y)==std::bit_cast<uint32_t>(actual.y) &&
                actual.event==binding.event && actual.action==binding.action,"fresh mapped input exactly matches native producer/raw consumer through original action curves"))return false;
            if(!check(!std::memcmp(&ctx,&saved,sizeof(ctx)) && !std::memcmp(memory.data(),base,memory.size()) && _mm_getcsr()==csr && GetLastError()==last_error,
                "full current context/TLS/stack/device arrays/action state/MXCSR/LastError restored"))return false;
            if(sample.x==0 && sample.y==0 && !check(actual.x==0 && actual.y==0,"neutral input remains neutral through authored curves"))return false;
            ++cases;
        }
    }
    setup(0,2,.2f,.95f,1.6f,.5f,0);Binding binding;
    if(!NativeCameraLookBindingFor(mode,base,binding))return false;
    {
        auto native=context();native.r3.u64=manager;native.r4.u64=action;native.r5.u64=0;native.r6.u64=0xA100;native.r7.u64=0xA104;
        const auto before=native;std::memcpy(memory.data(),base,memory.size());
        __imp__sub_827C5990(native,base);
        std::array<uint8_t,0x40000> expected_memory;std::memcpy(expected_memory.data(),base,expected_memory.size());
        const auto native_csr=_mm_getcsr();std::memcpy(base,memory.data(),memory.size());auto actual=before;
        sub_827C5990(actual,base);
        Binding rejected;
        if(!check(!NativeCameraLookBindingFor(mode,base,rejected) && !std::memcmp(&actual,&native,sizeof(actual)) &&
            !std::memcmp(expected_memory.data(),base,memory.size()) && _mm_getcsr()==native_csr,
            "actual disabled-action wrapper invalidates prior admission and preserves native full context/memory/FP"))return false;
    }
    setup(0,2,.2f,.95f,1.6f,.5f,0);NativeCameraLookBindingFor(mode,base,binding);
    for(unsigned irrelevant=0;irrelevant<64;++irrelevant){
        // Actual wrapper path: disabled unrelated native actions do not query
        // devices, but still exercise observation admission before original.
        const auto unrelated=0xB000u+irrelevant*512u;
        PPC_STORE_U32(unrelated+408,1);
        auto call=context();call.r3.u64=manager;call.r4.u64=unrelated;call.r5.u64=0;
        sub_827C5990(call,base);
        // Enabled irrelevant kinds are rejected by the observer too.
        ObserveAction(manager,unrelated,true,base);
    }
    {
        Binding retained;NativeCameraLookResult result;auto call=context();
        if(!check(NativeCameraLookBindingFor(mode,base,retained) && retained==binding &&
            ReplayNativeCameraLookAction(binding,{20000,-15000},call,base,result),
            "64 unrelated native action updates cannot evict a captured right-stick action"))return false;
        ObserveAction(manager,action,false,base);
        if(!check(!NativeCameraLookBindingFor(mode,base,retained) && !ReplayNativeCameraLookAction(binding,{20000,-15000},call,base,result),
            "known right-stick disable still invalidates admission after unrelated-action flood"))return false;
    }
    setup(0,2,.2f,.95f,1.6f,.5f,0);NativeCameraLookBindingFor(mode,base,binding);
    {
        constexpr uint32_t other_action=0xA400,selection_manager=0xA800;
        std::memcpy(base+other_action,base+action,436);PPC_STORE_U32(other_action,0xC001CB12);put(other_action+428,1.25f);
        ObserveAction(manager,other_action,true,base);ObserveNativeCameraLookBinding(mode,0x13572468,other_action,base);
        Binding selected;NativeCameraLookResult result;auto call=context();
        if(!check(NativeCameraLookBindingFor(mode,base,selected) && selected.action==action && selected==binding,
            "later eligible right-stick actions cannot replace the native-selected action or curve"))return false;
        PPC_STORE_U32(selection_manager+704,0xC001CB12);
        if(!check(!ReplayNativeCameraLookAction(binding,{20000,-15000},call,base,result) &&
            NativeCameraLookBindingFor(mode,base,selected) && selected.action==other_action && selected.event==0x13572468 &&
            ReplayNativeCameraLookAction(selected,{20000,-15000},call,base,result),
            "selection changes before replay reject old snapshots and find the previously observed correct action"))return false;
        PPC_STORE_U32(selection_manager+44,2);
        if(!check(NativeCameraLookBindingFor(mode,base,selected) && selected.action==action,
            "original native alternate-input selector switches back to the corresponding retained action"))return false;
        PPC_STORE_U32(controller+16,1);
        if(!check(!NativeCameraLookBindingFor(mode,base,selected) && !ReplayNativeCameraLookAction(binding,{20000,-15000},call,base,result),
            "secondary-user UID remapping cannot enter primary camera replay"))return false;
    }
    // Each mutable identity/range/curve component must force a fresh observation.
    for(unsigned guard=0;guard<15;++guard){
        setup(0,2,.2f,.95f,1.6f,.5f,0);NativeCameraLookBindingFor(mode,base,binding);
        if(guard==0)PPC_STORE_U32(0x833DA7E4,0xA004);
        if(guard==1)PPC_STORE_U32(controller+24,mode+4);
        if(guard==2)PPC_STORE_U32(mode+20,controller+4);
        if(guard==3)PPC_STORE_U32(manager+72,input_system+4);
        if(guard==4)PPC_STORE_U32(input_system+72,device+4);
        if(guard==5)PPC_STORE_U32(action,0xC001CAFF);
        if(guard==6)PPC_STORE_U32(action+408,3);
        if(guard==7)PPC_STORE_U32(action+92,0x01400003);
        if(guard==8)PPC_STORE_U8(action+104,1);
        if(guard==9)put(action+424,2.f);
        if(guard==10)PPC_STORE_U32(history+64+24,uint32_t(-32768));
        if(guard==11)ObserveAction(manager,action,false,base);
        if(guard==12)put(action+428,std::nanf(""));
        if(guard==13)PPC_STORE_U32(action+120,1);
        if(guard==14)PPC_STORE_U32(action+432,3);
        auto ctx=context();NativeCameraLookResult result;
        if(!check(!ReplayNativeCameraLookAction(binding,{20000,-15000},ctx,base,result) && result.x==0 && result.y==0,"identity/configuration/native enable changes reject stale snapshots without replay"))return false;
    }
    setup(0,2,.2f,.95f,1.6f,.5f,0);NativeCameraLookBindingFor(mode,base,binding);
    {
        AxisQuery scoped{manager,0,{.5f,-.25f}};QueryScope scope(scoped);
        for(unsigned guard=0;guard<5;++guard){auto ctx=context();ctx.lr=0x827C5A0C;ctx.r3.u64=manager;ctx.r4.u64=0x02400019;ctx.r5.u64=0;ctx.r6.u64=0;
            if(guard==0)ctx.lr=0;
            if(guard==1)ctx.r3.u64=manager+4;
            if(guard==2)ctx.r4.u64=0x01400003;
            if(guard==3)ctx.r5.u64=1;
            if(guard==4)ctx.r6.u64=1;
            const auto saved=ctx;if(!check(!AxisOverride(ctx) && !std::memcmp(&ctx,&saved,sizeof(ctx)),"scoped query cannot replace foreign callers/managers/users/keys/query modes"))return false;
        }
        auto ctx=context();NativeCameraLookResult result;
        if(!check(!RunMath(binding,{.5f,.5f},ctx,base,result) && query==&scoped,"nested action replay cannot replace an active query scope"))return false;
    }
    auto ctx=context();const auto saved=ctx;const auto csr=_mm_getcsr();const auto last_error=GetLastError();std::memcpy(memory.data(),base,memory.size());
    NativeCameraLookResult output;
    try{RunMath(binding,{.5f,-.25f},ctx,base,output,ThrowAfterNativeAction);return false;}catch(const std::runtime_error&){}
    if(!check(query==nullptr && !std::memcmp(&ctx,&saved,sizeof(ctx)) && !std::memcmp(base,memory.data(),memory.size()) && _mm_getcsr()==csr && GetLastError()==last_error,
        "native action exception restores full context/scratch/FP/environment and query scope"))return false;
    if(ok)REXLOG_INFO("Native camera look action passed {} real original action-handler cases / {} checks: two users, signed/diagonal/deadzone samples, native pow/radial and per-axis curves, binding inversion, exact raw-consumer comparison, changed identities/admission, no device/action writes, context/stack/TLS/FP restoration and nested/exception guards; full OnLook dispatch remains caller-owned",cases,checks);
    return ok;
}
