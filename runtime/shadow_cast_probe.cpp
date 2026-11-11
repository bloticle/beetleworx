#include "shadow_cast_probe.h"
#include "shadow_readback.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <string>
#include <windows.h>
#include <immintrin.h>

REXCVAR_DEFINE_INT32(em2_probe_shadow_mode,-1,"Debug","Private-only: force an existing active authored shadow mode 2 or 3 through the normal native depth-shadow path; no player/camera input");
REXCVAR_DEFINE_BOOL(em2_probe_opaque_shadow_casters,false,"Debug","Private-only: enable eligible opaque mesh casters during native shadow validation/render, restoring their flags afterward");
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(std::string,em2_user_data_directory);
PPC_EXTERN_FUNC(__imp__sub_8256C920);
PPC_EXTERN_FUNC(__imp__sub_825992C0);
PPC_EXTERN_FUNC(__imp__sub_825874E8);
PPC_EXTERN_FUNC(__imp__sub_825F5258);
namespace {
struct HostState {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~HostState(){SetLastError(error);_mm_setcsr(csr);}};
bool Readable(uint8_t* base,uint32_t address,size_t bytes){
    return base && address>=4096 && uint64_t(address)+bytes<=0x100000000ull &&
        em2::GuestReadBatch::Readable(PPC_RAW_ADDR(address),bytes);
}
uint32_t OpaqueProperty(uint8_t* base,uint32_t object){
    // This first caster policy admits only the inspected native NiMesh layout.
    // Unknown meshes and all blended/cutout materials retain authored behavior.
    if(!Readable(base,object,415) || PPC_LOAD_U32(object)!=0x8204A018 || PPC_LOAD_U8(object+414))return 0;
    const auto data=PPC_LOAD_U32(object+284);
    if(!Readable(base,data,384) || !(PPC_LOAD_U32(data+16)&0x400))return 0;
    const auto blend_property=PPC_LOAD_U8(data+116),blend=PPC_LOAD_U8(data+120);
    const auto test_property=PPC_LOAD_U8(data+152),test=PPC_LOAD_U8(data+156);
    if(blend_property>1 || blend>1 || test_property>1 || test>1 || (blend_property && blend) || (test_property && test))return 0;
    const auto mode=PPC_LOAD_U32(data+380);
    return mode==2 || mode==3?0:data;
}
bool TestOpaqueCasterPolicy();
class OpaqueCasterScope {
    struct Change {uint32_t object,data,flags;};
    uint8_t* base_=nullptr;std::vector<Change> changes_;
    friend bool TestOpaqueCasterPolicy();
    void Restore()noexcept{
        HostState restore;auto* base=base_;
        for(auto i=changes_.rbegin();i!=changes_.rend();++i)
            if(Readable(base,i->object,288) && PPC_LOAD_U32(i->object)==0x8204A018 && PPC_LOAD_U32(i->object+284)==i->data && Readable(base,i->data,20))
                PPC_STORE_U32(i->data+16,(PPC_LOAD_U32(i->data+16)&~0xC00u)|(i->flags&0xC00u));
    }
    void Apply(PPCContext& ctx,uint32_t object){
        auto* base=base_;const auto data=OpaqueProperty(base,object);if(!data)return;
        if(std::any_of(changes_.begin(),changes_.end(),[&](const auto& change){return change.data==data;}))return;
        const auto flags=PPC_LOAD_U32(data+16);
        changes_.push_back({object,data,flags}); // All host storage is reserved before writes.
        PPC_STORE_U32(data+16,flags&~0x400u);
        // The engine computes its derived cast bit and retains material-mode
        // restrictions. Do not synthesize an unconditional predicate result.
        em2::GuestReadBatch::CallbackBoundary boundary;
        ctx.r3.u64=object;__imp__sub_825874E8(ctx,base);
        if(!(PPC_LOAD_U32(data+16)&0x800)){
            PPC_STORE_U32(data+16,(PPC_LOAD_U32(data+16)&~0xC00u)|(flags&0xC00u));changes_.pop_back();
        }
    }
public:
    OpaqueCasterScope(PPCContext& ctx,uint8_t* base,uint32_t click):base_(base){
        if(!REXCVAR_GET(em2_probe_opaque_shadow_casters) || !REXCVAR_GET(em2_private_profile) || REXCVAR_GET(em2_user_data_directory).empty())return;
        HostState restore;const auto saved=ctx;struct ContextRestore {PPCContext& ctx;PPCContext saved;~ContextRestore(){ctx=saved;}} restore_context{ctx,saved};
        em2::GuestReadBatch inspection;
        if(!Readable(base,click,152) || PPC_LOAD_U32(click)!=0x8203EAA8 || !Readable(base,0x833E12AC,4))return;
        const auto owner=PPC_LOAD_U32(0x833E12AC),generator=PPC_LOAD_U32(click+140);
        if(!Readable(base,owner,225) || PPC_LOAD_U32(owner)!=0x820FAB5C || !PPC_LOAD_U8(owner+224) ||
           !Readable(base,generator,108) || PPC_LOAD_U32(generator)!=0x8203DEC0)return;
        const auto target=PPC_LOAD_U32(generator+84);bool owned=false;
        for(unsigned i=0;i<3;++i)owned|=target && PPC_LOAD_U32(owner+176+i*4)==target;
        if(!owned || !Readable(base,target,36) || PPC_LOAD_U32(target)!=0x8203D708 || PPC_LOAD_U32(target+24)!=click)return;
        const auto slot=PPC_LOAD_U32(target+32),source=PPC_LOAD_U32(target+28);
        if(slot<2 || slot>4 || !Readable(base,source,204329) || PPC_LOAD_U32(source+8)!=0x8203EB5C)return;
        const auto last=PPC_LOAD_U32(source+204320);if(last>=6000)return;
        const auto mask=uint16_t(0x20u<<slot);
        try{
            changes_.reserve(size_t(last)+1);
            for(unsigned i=0;i<=last;++i){
                const auto flags=PPC_LOAD_U16(source+192292+i*2);
                if((flags&mask) && !(flags&1024))Apply(ctx,PPC_LOAD_U32(source+36+i*4));
            }
            if(!changes_.empty()){
                static std::atomic<bool> reported{false};
                if(!reported.exchange(true))REXLOG_INFO("Private opaque shadow casters: {} eligible native mesh property states enabled for slot {}; original validator/material renderer and alpha state retained, shadow bits restored after each native call",changes_.size(),slot);
            }
        }catch(...){Restore();changes_.clear();}
    }
    ~OpaqueCasterScope(){Restore();}
    OpaqueCasterScope(const OpaqueCasterScope&)=delete;
};
uint32_t AuthoredLight(uint8_t* base,uint32_t selector,unsigned mode){
    em2::GuestReadBatch inspection;
    if((mode!=2 && mode!=3) || !Readable(base,selector,12) || PPC_LOAD_U32(selector)!=0x821DF028)return 0;
    const auto collection=PPC_LOAD_U32(selector+8);
    if(!Readable(base,collection,36))return 0;
    auto node=PPC_LOAD_U32(collection+32);std::array<uint32_t,128> visited{};unsigned count=0;
    while(node){
        if(count==visited.size() || std::find(visited.begin(),visited.begin()+count,node)!=visited.begin()+count || !Readable(base,node,96))return 0;
        visited[count++]=node;
        if(PPC_LOAD_U32(node+80)==mode){
            const auto light=PPC_LOAD_U32(node+76);
            // The probe uses only the authored 2D spotlight path. Cube/unknown
            // generators need a different texture contract and are untouched.
            if(Readable(base,light,336) && PPC_LOAD_U32(light)==0x8203E1D8 && PPC_LOAD_U8(light+240)){
                bool nonzero=false,valid=true;
                for(unsigned i=0;i<3;++i){const auto bits=PPC_LOAD_U32(light+324+i*4);valid&=(bits&0x7F800000)!=0x7F800000;nonzero|=(bits&0x7FFFFFFF)!=0;}
                if(valid && nonzero)return light;
            }
        }
        node=PPC_LOAD_U32(node+4);
    }
    return 0;
}
void StoreSelection(uint8_t* base,uint32_t selected,const std::array<uint32_t,3>& value){for(unsigned i=0;i<3;++i)PPC_STORE_U32(selected+i*4,value[i]);}
bool TestOpaqueCasterPolicy(){
    HostState restore;std::vector<uint8_t> memory(0x20000);auto* base=memory.data();
    constexpr uint32_t object=0x10000,data=0x11000;
    PPC_STORE_U32(object,0x8204A018);PPC_STORE_U32(object+284,data);PPC_STORE_U32(data+16,0x600);
    PPC_STORE_U8(data+116,1);PPC_STORE_U8(data+152,1);
    auto check=[](bool value,const char* description){if(!value)REXLOG_ERROR("Opaque shadow caster fixture: {}",description);return value;};
    if(!check(OpaqueProperty(base,object)==data,"opaque authored no-shadow mesh admitted"))return false;
    for(const auto field:{120u,156u}){
        PPC_STORE_U8(data+field,1);if(!check(!OpaqueProperty(base,object),"blended/cutout mesh excluded"))return false;PPC_STORE_U8(data+field,0);
    }
    PPC_STORE_U8(object+414,1);if(!check(!OpaqueProperty(base,object),"disabled mesh excluded"))return false;PPC_STORE_U8(object+414,0);
    for(unsigned mode:{2u,3u}){PPC_STORE_U32(data+380,mode);if(!check(!OpaqueProperty(base,object),"native material-mode exclusion retained"))return false;}PPC_STORE_U32(data+380,0);
    PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r3.u64=object;ctx.r4.u64=3;ctx.fpscr.InitHost();
    __imp__sub_825F5258(ctx,base);if(!check(!ctx.r3.u32,"original native shadow predicate rejects authored flag"))return false;
    // Execute the original code to establish the alpha restriction. In
    // 82587070 the blend branch returns ONE, not zero: do not bypass it or
    // promise alpha-correct shadows by forcing the derived cast bit.
    for(unsigned blend:{0u,1u})for(unsigned test:{0u,1u}){
        PPC_STORE_U32(data+16,0x200);PPC_STORE_U8(data+120,blend);PPC_STORE_U8(data+156,test);PPC_STORE_U32(data+180,4);
        ctx.r3.u64=object;__imp__sub_825874E8(ctx,base);
        if(!check(bool(PPC_LOAD_U32(data+16)&0x800)==(!blend && !test),"original native recompute rejects blending and alpha testing independently"))return false;
    }
    PPC_STORE_U8(data+120,0);PPC_STORE_U8(data+156,0);PPC_STORE_U32(data+180,0);PPC_STORE_U32(data+16,0x600);

    try{
        OpaqueCasterScope scope(ctx,nullptr,0);scope.base_=base;scope.changes_.reserve(1);scope.Apply(ctx,object);
        ctx.r3.u64=object;ctx.r4.u64=3;__imp__sub_825F5258(ctx,base);
        if(!check(ctx.r3.u32==1 && (PPC_LOAD_U32(data+16)&0xC00)==0x800,"original native recompute and predicate admit the opaque caster"))return false;
        PPC_STORE_U32(data+16,PPC_LOAD_U32(data+16)|0x02000000);throw std::runtime_error("caster scope unwind");
    }catch(const std::runtime_error&){}
    if(!check(PPC_LOAD_U32(data+16)==0x02000600,"unwind restores only shadow ownership bits and preserves other native state changes"))return false;
    REXLOG_INFO("Opaque shadow caster checks passed: opaque admission, blend/cutout/disabled/material-mode exclusions, actual native eligibility recomputation/predicate and scoped restoration on unwind; live caster pixels remain acceptance");return true;
}
}
PPC_FUNC_IMPL(sub_8256C920){OpaqueCasterScope casters(ctx,base,ctx.r4.u32);__imp__sub_8256C920(ctx,base);}
PPC_FUNC_IMPL(sub_825992C0){const auto click=ctx.r3.u32;OpaqueCasterScope casters(ctx,base,click);__imp__sub_825992C0(ctx,base);em2::ProbeNativeShadowPixels(base,click);}
em2::NativeShadowCastProbe::NativeShadowCastProbe(uint32_t caller,uint32_t owner,uint32_t selected,uint8_t* base){
    const auto mode=REXCVAR_GET(em2_probe_shadow_mode);
    if((mode!=2 && mode!=3) || caller!=0x827A59D4 || !REXCVAR_GET(em2_private_profile) || REXCVAR_GET(em2_user_data_directory).empty())return;
    HostState restore;GuestReadBatch inspection;
    if(!Readable(base,owner,225) || PPC_LOAD_U32(owner)!=0x820FAB5C || !PPC_LOAD_U8(owner+224) || !Readable(base,selected,12) || !Readable(base,0x833DB1F0,4))return;
    MEMORY_BASIC_INFORMATION input_region{};
    if(!VirtualQuery(PPC_RAW_ADDR(selected),&input_region,sizeof(input_region)) ||
       !(input_region.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return;
    for(unsigned i=0;i<3;++i)original_[i]=PPC_LOAD_U32(selected+i*4);
    if(std::any_of(original_.begin(),original_.end(),[](auto value){return value!=0;}))return;
    const auto manager=PPC_LOAD_U32(0x833DB1F0);
    if(!Readable(base,manager,24))return;
    const auto light=AuthoredLight(base,PPC_LOAD_U32(manager+20),unsigned(mode));if(!light)return;
    static std::atomic<uint32_t> last_light{0};
    if(last_light.exchange(light)!=light)REXLOG_INFO("Private shadow-cast probe: authored mode {} light {:08X} supplied to the original native preparation path; input restored afterward, no player/camera changes",mode,light);
    // No fallible host allocation/logging after publishing the temporary input:
    // a constructor exception must not leave an unowned override behind.
    base_=base;selected_=selected;StoreSelection(base_,selected_,{light,0,0});
}
em2::NativeShadowCastProbe::~NativeShadowCastProbe(){
    if(selected_){HostState restore;auto* base=base_;StoreSelection(base,selected_,original_);}
}
bool em2::TestNativeShadowCastProbe(){
    if(!TestOpaqueCasterPolicy())return false;
    HostState restore;std::vector<uint8_t> memory(0x20000);auto* base=memory.data();
    constexpr uint32_t selector=0x10000,collection=0x11000,node=0x12000,light=0x13000,selected=0x14000;
    PPC_STORE_U32(selector,0x821DF028);PPC_STORE_U32(selector+8,collection);PPC_STORE_U32(collection+32,node);
    PPC_STORE_U32(node+76,light);PPC_STORE_U32(node+80,2);PPC_STORE_U32(light,0x8203E1D8);PPC_STORE_U8(light+240,1);PPC_STORE_U32(light+324,0x3F800000);
    auto check=[](bool valid,const char* description){if(!valid)REXLOG_ERROR("Native shadow-cast probe fixture: {}",description);return valid;};
    if(!check(AuthoredLight(base,selector,2)==light && !AuthoredLight(base,selector,3),"exact authored mode and active spotlight"))return false;
    PPC_STORE_U8(light+240,0);if(!check(!AuthoredLight(base,selector,2),"inactive light rejected"))return false;PPC_STORE_U8(light+240,1);
    PPC_STORE_U32(light+324,0x7FC00000);if(!check(!AuthoredLight(base,selector,2),"nonfinite colour rejected"))return false;PPC_STORE_U32(light+324,0x3F800000);
    PPC_STORE_U32(light,0x8203E2E8);if(!check(!AuthoredLight(base,selector,2),"cube/unknown light not forced through the 2D map path"))return false;PPC_STORE_U32(light,0x8203E1D8);
    PPC_STORE_U32(node+4,node);if(!check(!AuthoredLight(base,selector,3),"cycle rejected"))return false;PPC_STORE_U32(node+4,0);
    const auto original=memory;const auto mode=REXCVAR_GET(em2_probe_shadow_mode);
    struct RestoreMode {int value;~RestoreMode(){REXCVAR_SET(em2_probe_shadow_mode,value);}} restore_mode{mode};
    REXCVAR_SET(em2_probe_shadow_mode,-1);{NativeShadowCastProbe probe(0,0,0,nullptr);}
    if(!check(memory==original,"disabled probe does not access or modify guest input"))return false;
    try{
        NativeShadowCastProbe probe(0,0,0,nullptr);probe.base_=base;probe.selected_=selected;probe.original_={0,0,0};
        StoreSelection(base,selected,{light,0,0});throw std::runtime_error("fixture unwind");
    }catch(const std::runtime_error&){}
    if(!check(memory==original,"actual probe destructor restores all input bytes on unwind"))return false;
    REXLOG_INFO("Native shadow-cast probe checks passed: authored-mode matching, inactive/nonfinite/cube rejection, bounded cyclic-list rejection and disabled isolation/actual selection restoration on unwind; live original generator creation remains a GPU check");return true;
}
