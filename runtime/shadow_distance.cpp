#include "shadow_distance.h"
#include "shadow_resize.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <vector>
#include <windows.h>
#include <immintrin.h>

REXCVAR_DEFINE_BOOL(em2_distance_shadows,false,"GPU","Experimental distance-based resolution for native environment point/spot shadow maps; directional/player maps retain configured quality");
REXCVAR_DEFINE_INT32(em2_distance_shadow_quality,1,"GPU","Local-light shadow distance preset: 0 quality, 1 balanced, 2 performance");
namespace {
struct HostState {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~HostState(){SetLastError(error);_mm_setcsr(csr);}};
struct Bands {float half,quarter,restore_full,restore_half;};
constexpr std::array<Bands,3> presets{{{50,100,40,85},{25,60,20,50},{15,35,12,28}}};
unsigned Preset(){const auto value=REXCVAR_GET(em2_distance_shadow_quality);return value>=0 && value<int(presets.size())?unsigned(value):0;}
struct Selection {
    unsigned level=0;
    int64_t changed_at=0;
    unsigned Update(float distance,int64_t now,const Bands& bands=presets[1]){
        if(!std::isfinite(distance) || distance<0){level=0;changed_at=now;return level;}
        // Asymmetric boundaries prevent repeated allocations around a band.
        unsigned next=level;
        if(level==0){if(distance>bands.quarter)next=2;else if(distance>bands.half)next=1;}
        else if(level==1){if(distance<bands.restore_full)next=0;else if(distance>bands.quarter)next=2;}
        else {if(distance<bands.restore_full)next=0;else if(distance<bands.restore_half)next=1;}
        // Restore detail immediately when approaching a light. Only reductions
        // wait, so rapid camera/character movement never waits for sharper maps.
        if(next<level || (next>level && now>=changed_at && now-changed_at>=1000)){
            level=next;changed_at=now;
        }
        return level;
    }
};
struct State {
    uint32_t owner=0;
    unsigned preset=1;
    std::array<uint32_t,3> lights{};
    std::array<Selection,3> choices{};
};
State state;
std::atomic<bool> policy_active{false};
std::mutex state_mutex;
bool Readable(uint8_t* base,uint32_t address,size_t bytes){
    return base && address>=4096 && uint64_t(address)+bytes<=0x100000000ull &&
        em2::GuestReadBatch::Readable(PPC_RAW_ADDR(address),bytes);
}
using Position=std::array<float,3>;
bool PositionAt(uint8_t* base,uint32_t address,Position& position){
    if(!Readable(base,address,12))return false;
    for(unsigned i=0;i<3;++i){position[i]=std::bit_cast<float>(PPC_LOAD_U32(address+i*4));if(!std::isfinite(position[i]))return false;}
    return true;
}
std::array<unsigned,3> Levels(const State& value){return {value.choices[0].level,value.choices[1].level,value.choices[2].level};}
std::array<uint32_t,3> Sizes(std::array<uint32_t,3> maximum,const std::array<unsigned,3>& levels){
    for(unsigned i=1;i<3;++i)if(maximum[i]>=128 && maximum[i]<=2048 && std::has_single_bit(maximum[i]) && levels[i]<=2)maximum[i]=std::max(128u,maximum[i]>>levels[i]);
    return maximum;
}
}
void em2::ConfigureNativeShadowDistance(unsigned choice){
    HostState restore;
    if(!choice || choice>presets.size()){REXCVAR_SET(em2_distance_shadows,false);return;}
    REXCVAR_SET(em2_distance_shadow_quality,int32_t(choice-1));
    REXCVAR_SET(em2_distance_shadows,true);
}
unsigned em2::NativeShadowDistanceSetting(){HostState restore;return REXCVAR_GET(em2_distance_shadows)?Preset()+1:0;}
void em2::UpdateNativeShadowDistance(uint32_t caller,uint32_t owner,uint32_t selected,uint32_t camera_count,uint32_t cameras,uint8_t* base){
    HostState restore;
    const bool enabled=REXCVAR_GET(em2_distance_shadows) && caller==0x827A59D4;
    if(!enabled && !policy_active.load(std::memory_order_acquire))return;
    // The other native entrypoints don't supply this camera contract. They
    // still request their configured maximums after disabling the feature.
    GuestReadBatch inspection;
    std::array<float,3> distance{};
    distance.fill(std::numeric_limits<float>::quiet_NaN());
    std::array<uint32_t,3> lights{};
    if(enabled && Readable(base,owner,225) && PPC_LOAD_U32(owner)==0x820FAB5C && PPC_LOAD_U8(owner+224) && Readable(base,selected,12)){
        std::array<Position,64> views{};size_t count=0;
        bool valid=camera_count>0 && camera_count<=views.size() && Readable(base,cameras,size_t(camera_count)*4);
        auto append=[&](uint32_t view){
            if(!Readable(base,view,204) || PPC_LOAD_U32(view)!=0x8203E610 || count>=views.size() || !PositionAt(base,view+192,views[count]))return false;
            ++count;return true;
        };
        for(unsigned i=0;i<camera_count && valid;++i)if(!append(PPC_LOAD_U32(cameras+i*4)))valid=false;
        std::array<bool,3> full_quality{};
        // Selection order is NOT texture-slot order. The native ShadowMapID
        // selects each view's actual target. Follow that retained binding so
        // an environment light selected first cannot resize the player map.
        if(valid && count)for(unsigned i=0;i<3;++i){
            const auto view=PPC_LOAD_U32(owner+164+i*4);
            if(!Readable(base,view,108) || PPC_LOAD_U32(view)!=0x8203DEC0)continue;
            // The native constructor stores views by ShadowMapID as well.
            // A selected light at index zero can therefore own view one.
            const auto light=PPC_LOAD_U32(view+80);
            bool selected_light=false;
            for(unsigned candidate=0;candidate<3;++candidate)selected_light|=light && PPC_LOAD_U32(selected+candidate*4)==light;
            if(!selected_light)continue;
            const auto target=PPC_LOAD_U32(view+84);unsigned slot=3;
            for(unsigned candidate=0;candidate<3;++candidate)if(target && target==PPC_LOAD_U32(owner+176+candidate*4)){slot=candidate;break;}
            if(slot==3){
                // Publication precedes the next native generator refresh. The
                // old retained target remains owned during that interval; do
                // not mistake it for a foreign binding and undo the resize.
                const auto tracked=TrackedShadowTargetSlot(owner,target);
                if(tracked>=0 && tracked<3)slot=unsigned(tracked);
            }
            if(slot==0 || slot==3)continue;
            lights[slot]=light;Position position;
            if(!Readable(base,light,204))continue;
            const auto type=PPC_LOAD_U32(light);
            if((type!=0x8203E1D8 && type!=0x8203E2E8) || !PositionAt(base,light+192,position)){full_quality[slot]=true;continue;}
            double nearest=std::numeric_limits<double>::infinity();
            for(size_t v=0;v<count;++v){double squared=0;for(unsigned axis=0;axis<3;++axis){const auto delta=double(position[axis])-views[v][axis];squared+=delta*delta;}nearest=std::min(nearest,squared);}
            const auto measured=std::sqrt(nearest);
            if(measured<=std::numeric_limits<float>::max())distance[slot]=std::isfinite(distance[slot])?std::min(distance[slot],float(measured)):float(measured);
        }
        for(unsigned slot=1;slot<3;++slot)if(full_quality[slot])distance[slot]=std::numeric_limits<float>::quiet_NaN();
    }
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto preset=Preset();
    bool changed=false;
    {std::lock_guard lock(state_mutex);
        const auto previous=Levels(state);const auto previous_owner=state.owner;
        if(!enabled)state={};
        else {
            if(state.owner!=owner){state={};state.owner=owner;for(auto& choice:state.choices)choice.changed_at=now;}
            if(state.preset!=preset){state.preset=preset;for(auto& choice:state.choices)choice.changed_at=now;}
            for(unsigned i=0;i<3;++i){
                if(state.lights[i]!=lights[i]){state.choices[i]={0,now};state.lights[i]=lights[i];}
                state.choices[i].Update(distance[i],now,presets[preset]);
            }
        }
        changed=previous!=Levels(state) || (previous_owner!=state.owner && previous!=std::array<unsigned,3>{});
        policy_active.store(enabled && state.owner,std::memory_order_release);
    }
    if(changed)QueueShadowResize();
}
std::array<uint32_t,3> em2::ShadowDistanceTargetSizes(uint32_t owner,std::array<uint32_t,3> maximum){
    HostState restore;std::lock_guard lock(state_mutex);
    return state.owner==owner?Sizes(maximum,Levels(state)):maximum;
}
bool em2::TestShadowDistancePolicy(){
    HostState restore;
    auto check=[](bool valid,const char* message){if(!valid)REXLOG_ERROR("Shadow distance policy: {}",message);return valid;};
    Selection choice;
    if(!check(choice.Update(30,999)==0 && choice.Update(30,1000)==1,"reduction waits one second") ||
       !check(choice.Update(24,1100)==1 && choice.Update(20,1200)==1,"near-band hysteresis") ||
       !check(choice.Update(19,1201)==0,"near detail restored immediately") ||
       !check(choice.Update(80,2200)==0 && choice.Update(80,2201)==2,"far reduction cooldown") ||
       !check(choice.Update(55,2300)==2 && choice.Update(49,2301)==1,"far-band hysteresis") ||
       !check(choice.Update(5,2302)==0,"teleport directly restores full detail"))return false;
    for(const auto value:{-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
        choice={2,0};if(!check(choice.Update(value,10000)==0,"invalid input falls back to full quality"))return false;
    }
    choice={0,10000};if(!check(choice.Update(80,5000)==0,"backwards clock never reduces detail"))return false;
    for(unsigned distance=0;distance<=600;++distance){
        std::array<unsigned,3> levels{};
        for(unsigned i=0;i<3;++i){Selection selection;levels[i]=selection.Update(float(distance)*.25f,1000,presets[i]);}
        if(!check(levels[0]<=levels[1] && levels[1]<=levels[2],"quality presets never reduce detail more than a faster preset"))return false;
    }
    for(const auto& bands:presets){
        Selection selection;
        if(!check(selection.Update(bands.half+.1f,999,bands)==0 && selection.Update(bands.half+.1f,1000,bands)==1,"preset reduction waits for cooldown") ||
           !check(selection.Update((bands.restore_full+bands.half)*.5f,1001,bands)==1 && selection.Update(bands.restore_full-.1f,1002,bands)==0,"preset near-band hysteresis and immediate detail restoration") ||
           !check(selection.Update(bands.quarter+.1f,2002,bands)==2 && selection.Update(bands.restore_half+.1f,2003,bands)==2 && selection.Update(bands.restore_half-.1f,2004,bands)==1,"preset far-band hysteresis"))return false;
    }
    std::array<unsigned,3> middle{};
    for(unsigned i=0;i<3;++i){Selection selection;middle[i]=selection.Update(40,1000,presets[i]);}
    if(!check(middle==std::array<unsigned,3>{0,1,2},"quality/balanced/performance select full/half/quarter detail at forty units"))return false;
    const std::array<uint32_t,3> maximum={512,2048,1024};
    if(!check(Sizes(maximum,{2,1,2})==std::array<uint32_t,3>{512,1024,256},"player slot unchanged; per-environment-map limits respected") ||
       !check(Sizes({128,128,256},{2,2,2})==std::array<uint32_t,3>{128,128,128},"minimum native target dimension") ||
       !check(Sizes(maximum,{0,99,99})==maximum,"invalid levels retain configured sizes"))return false;
    const auto previous_enabled=REXCVAR_GET(em2_distance_shadows);State previous_state;
    const auto previous_quality=REXCVAR_GET(em2_distance_shadow_quality);
    const bool previous_active=policy_active.load();
    {std::lock_guard lock(state_mutex);previous_state=state;}
    struct RestorePolicy {bool enabled,active;int quality;State saved;~RestorePolicy(){
        REXCVAR_SET(em2_distance_shadows,enabled);REXCVAR_SET(em2_distance_shadow_quality,quality);std::lock_guard lock(state_mutex);state=saved;policy_active=active;
    }} restore_policy{previous_enabled,previous_active,previous_quality,previous_state};
    REXCVAR_SET(em2_distance_shadow_quality,1);
    std::vector<uint8_t> memory(0x40000);auto* base=memory.data();
    constexpr uint32_t owner=0x10000,selected=0x11000,camera=0x20000,near_camera=0x21000,cameras=0x12000;
    constexpr uint32_t light=0x22000,view=0x24000,target=0x29000;
    PPC_STORE_U32(owner,0x820FAB5C);PPC_STORE_U8(owner+224,1);
    PPC_STORE_U32(selected,light);PPC_STORE_U32(owner+164,view);
    PPC_STORE_U32(owner+180,target); // Selected light zero targets ENVIRONMENT slot one.
    PPC_STORE_U32(view,0x8203DEC0);PPC_STORE_U32(view+80,light);PPC_STORE_U32(view+84,target);
    PPC_STORE_U32(camera,0x8203E610);PPC_STORE_U32(near_camera,0x8203E610);
    PPC_STORE_U32(cameras,camera);PPC_STORE_U32(cameras+4,0xDEADBEEF); // Counted, not NUL-terminated.
    PPC_STORE_U32(near_camera+192,std::bit_cast<uint32_t>(68.f));
    PPC_STORE_U32(light,0x8203E1D8);PPC_STORE_U32(light+192,std::bit_cast<uint32_t>(70.f));
    auto seed=[&](unsigned level){std::lock_guard lock(state_mutex);state={};state.owner=owner;state.lights[1]=light;state.choices[1].level=level;};
    REXCVAR_SET(em2_distance_shadows,true);seed(0);
    const auto initial_memory=memory;const auto initial_csr=_mm_getcsr();SetLastError(0x11223344);
    UpdateNativeShadowDistance(0x827A59D4,owner,selected,1,cameras,base);
    if(!check(memory==initial_memory && _mm_getcsr()==initial_csr && GetLastError()==0x11223344,"native policy leaves all guest bytes and host FP/error state unchanged"))return false;
    if(!check(ShadowDistanceTargetSizes(owner,maximum)==std::array<uint32_t,3>{512,512,1024},"real native view/target binding overrides selected-array order"))return false;
    ConfigureNativeShadowDistance(1);
    UpdateNativeShadowDistance(0x827A59D4,owner,selected,1,cameras,base);
    if(!check(ShadowDistanceTargetSizes(owner,maximum)==std::array<uint32_t,3>{512,1024,1024},"higher-quality preset immediately restores detail"))return false;
    ConfigureNativeShadowDistance(3);
    UpdateNativeShadowDistance(0x827A59D4,owner,selected,1,cameras,base);
    if(!check(ShadowDistanceTargetSizes(owner,maximum)==std::array<uint32_t,3>{512,1024,1024},"lower-quality preset does not bypass reduction cooldown"))return false;
    {std::lock_guard lock(state_mutex);state.choices[1].changed_at-=1100;}
    UpdateNativeShadowDistance(0x827A59D4,owner,selected,1,cameras,base);
    if(!check(ShadowDistanceTargetSizes(owner,maximum)==std::array<uint32_t,3>{512,512,1024},"lower-quality preset reduces after settling"))return false;
    REXCVAR_SET(em2_distance_shadow_quality,1);
    PPC_STORE_U32(owner+164,0);PPC_STORE_U32(owner+168,view);seed(0);
    UpdateNativeShadowDistance(0x827A59D4,owner,selected,1,cameras,base);
    if(!check(ShadowDistanceTargetSizes(owner,maximum)==std::array<uint32_t,3>{512,512,1024},"selected index zero with native view index one still reduces the bound environment map"))return false;
    PPC_STORE_U32(selected,0);seed(2);
    UpdateNativeShadowDistance(0x827A59D4,owner,selected,1,cameras,base);
    if(!check(ShadowDistanceTargetSizes(owner,maximum)==maximum,"retained view for an unselected light restores full quality"))return false;
    PPC_STORE_U32(selected,light);
    PPC_STORE_U32(cameras+4,near_camera);
    UpdateNativeShadowDistance(0x827A59D4,owner,selected,2,cameras,base);
    if(!check(ShadowDistanceTargetSizes(owner,maximum)==maximum,"nearest additional view immediately restores full detail"))return false;
    seed(2);PPC_STORE_U32(light,0x8203E3D8); // Directional light position is not a local-light distance.
    UpdateNativeShadowDistance(0x827A59D4,owner,selected,1,cameras,base);
    if(!check(ShadowDistanceTargetSizes(owner,maximum)==maximum,"directional maps retain full quality"))return false;
    seed(2);PPC_STORE_U32(camera,0xDEADBEEF);
    UpdateNativeShadowDistance(0x827A59D4,owner,selected,1,cameras,base);
    if(!check(ShadowDistanceTargetSizes(owner,maximum)==maximum,"unknown camera falls back to full quality"))return false;
    for(unsigned invalid_count:{0u,65u}){
        seed(2);UpdateNativeShadowDistance(0x827A59D4,owner,selected,invalid_count,cameras,base);
        if(!check(ShadowDistanceTargetSizes(owner,maximum)==maximum,"invalid counted-camera extent retains full quality"))return false;
    }
    seed(2);UpdateNativeShadowDistance(0x829BB338,owner,selected,0,0,base);
    if(!check(ShadowDistanceTargetSizes(owner,maximum)==maximum,"native cleanup/manual selection without the camera contract restores full quality"))return false;
    seed(2);PPC_STORE_U32(camera,0x8203E610);PPC_STORE_U32(light,0x8203E1D8);
    PPC_STORE_U32(view+84,0x38000);
    UpdateNativeShadowDistance(0x827A59D4,owner,selected,1,cameras,base);
    if(!check(ShadowDistanceTargetSizes(owner,maximum)==maximum,"foreign target binding cannot reduce an owned map"))return false;
    seed(2);REXCVAR_SET(em2_distance_shadows,false);
    UpdateNativeShadowDistance(0,0,0,0,0,nullptr);
    if(!check(ShadowDistanceTargetSizes(owner,maximum)==maximum && ShadowDistanceTargetSizes(owner+4,maximum)==maximum,"disable and foreign owner restore configured sizes"))return false;
    REXLOG_INFO("Shadow distance policy passed: delay/hysteresis, immediate near restoration, teleport, invalid/backwards inputs, independent map dimensions and player quality preservation; live caster/render acceptance remains required");return true;
}
