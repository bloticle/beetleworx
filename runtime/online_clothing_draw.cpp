#include "online_clothing_draw.h"
#include "online_clothing_native.h"
#include "online_clothing_texture.h"
#include "online_lobby.h"
#include "online_room_gameplay.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <array>
#include <vector>
#include <algorithm>
#include <optional>
#include <tuple>
#include <filesystem>
#include <fstream>
#include <exception>
#include <windows.h>
#include <immintrin.h>

REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_native_room_lobby);
REXCVAR_DEFINE_BOOL(em2_trace_online_clothing,false,"Debug","Private native Room: bounded read-only player mesh/material/texture ownership capture; no material changes");
REXCVAR_DEFINE_BOOL(em2_online_clothing_bind,false,"Debug","Private native Room candidate: cached per-player default pants texture, original diffuse restored after each draw");
REXCVAR_DECLARE(std::string,em2_probe_online_clothing_directory);
REXCVAR_DECLARE(bool,em2_online_name_tags);
REXCVAR_DECLARE(bool,em2_room_probe_scripted_input);
REXCVAR_DEFINE_BOOL(em2_probe_online_clothing_lifecycle,false,"Debug","Isolated scripted Room only: exercise live pants binding disable/release/reload, including hidden name tags");
namespace em2::net {
namespace {
struct Owner {uint64_t peer=0;uint32_t entity=0,root=0,rgba=0;unsigned character=0;};
thread_local std::vector<Owner> roots;
thread_local std::vector<std::tuple<uint32_t,uint32_t,uint32_t>> seen;
thread_local std::array<NativeClothingTexture,32> colors;
thread_local std::string color_directory;
thread_local bool color_failed=false;
thread_local unsigned color_draw_depth=0;
thread_local std::vector<std::tuple<uint64_t,uint32_t,uint32_t>> color_reported;
struct HostState {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~HostState(){_mm_setcsr(fp);SetLastError(error);}};
bool Enabled(){return (REXCVAR_GET(em2_private_profile) || REXCVAR_GET(em2_native_room_lobby)) && (REXCVAR_GET(em2_trace_online_clothing) || REXCVAR_GET(em2_online_clothing_bind));}
std::string SourceKey(){const auto directory=REXCVAR_GET(em2_probe_online_clothing_directory);return directory.empty()?OnlineClothingSourceKey():"directory:"+directory;}
bool Readable(uint32_t at,size_t bytes,uint8_t* base){return base && at>=4096 && uint64_t(at)+bytes<=0x100000000ull && GuestReadBatch::Readable(base+at,bytes);}
std::optional<Owner> Find(uint32_t mesh,uint8_t* base){
    uint32_t current=mesh;std::array<uint32_t,64> visited{};unsigned depth=0;
    while(current && depth<visited.size()){
        if(!Readable(current,60,base) || std::find(visited.begin(),visited.begin()+depth,current)!=visited.begin()+depth)return {};
        visited[depth++]=current;
        const auto found=std::find_if(roots.begin(),roots.end(),[&](const auto& owner){return owner.root==current;});
        if(found!=roots.end())return *found;
        current=PPC_LOAD_U32(current+56);
    }
    return {};
}
uint32_t Type(uint32_t object,uint8_t* base){
    if(!Readable(object,16,base))return 0;
    const auto table=PPC_LOAD_U32(object);if(!Readable(table,12,base))return 0;
    return PPC_LOAD_U32(table+8);
}
uint32_t Physical(uint32_t texture,uint8_t* base){
    if(!Readable(texture,44,base))return 0;const auto data=PPC_LOAD_U32(texture+40);
    if(!Readable(data,96,base) || PPC_LOAD_U32(data)!=0x82054F10)return 0;
    // This is the D3D texture header itself: 82672450 decrements its +4
    // reference count and submits it directly to the resource manager. It is
    // not the separate 40-byte allocation wrapper used during texture creation.
    const auto header=PPC_LOAD_U32(data+92);if(!Readable(header,52,base) || (PPC_LOAD_U32(header)&15)!=3)return 0;
    // Same native fetch layout used by shadow_readback.cpp: six fetch words
    // begin at +28; the base-address word is fetch1 at +32.
    return PPC_LOAD_U32(header+32)&0x1FFFF000;
}

}
bool ReadNativeClothingBinding(uint32_t mesh,uint8_t* base,NativeClothingBinding& out,unsigned* diagnostic){
    HostState preserve;GuestReadBatch reads;unsigned current=0;auto stage=[&]{if(diagnostic)*diagnostic=++current;};NativeClothingBinding b;
    stage();if(!Readable(mesh,244,base))return false;b.state=PPC_LOAD_U32(mesh+240);
    stage();if(!Readable(b.state,44,base))return false;b.property=PPC_LOAD_U32(b.state+40);
    stage();if(Type(b.property,base)!=0x8259E620 || !Readable(b.property,52,base))return false;b.shader_array=PPC_LOAD_U32(b.property+48);
    stage();if(!Readable(b.shader_array,16,base) || PPC_LOAD_U32(b.shader_array)!=0x821D4FD8)return false;
    const auto capacity=PPC_LOAD_U16(b.shader_array+8),count=PPC_LOAD_U16(b.shader_array+10),filled=PPC_LOAD_U16(b.shader_array+12);
    const auto entries=PPC_LOAD_U32(b.shader_array+4);
    if(count<3 || count>16 || capacity<count || capacity>32 || filled>count || !Readable(entries,unsigned(count)*4,base))return false;
    for(unsigned slot=0;slot<3;++slot){
        stage();b.maps[slot]=PPC_LOAD_U32(entries+slot*4);
        // The array position selects diffuse/normal/specular. The extra word
        // at +16 is zero in ALL five authored default shader maps, not a slot
        // index. Retain that audited layout rather than inventing map IDs.
        if(!Readable(b.maps[slot],20,base) || PPC_LOAD_U32(b.maps[slot])!=0x8204373C || PPC_LOAD_U32(b.maps[slot]+16)!=0)return false;
        b.textures[slot]=PPC_LOAD_U32(b.maps[slot]+8);
        stage();if(Type(b.textures[slot],base)!=0x82585D00)return false;
    }
    if(b.maps[0]==b.maps[1] || b.maps[0]==b.maps[2] || b.maps[1]==b.maps[2] || b.textures[0]==b.textures[1] || b.textures[0]==b.textures[2] || b.textures[1]==b.textures[2])return false;
    out=b;return true;
}
void BeginOnlineClothingDrawFrame(PPCContext& ctx,uint8_t* base,bool available){
    HostState preserve;
    // Always retire the last frame's actor identities, including when a flag
    // has just been disabled. Never reuse a previous session's native roots.
    roots.clear();
    if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_room_probe_scripted_input) &&
       REXCVAR_GET(em2_probe_online_clothing_lifecycle) && available){
        auto* game=CurrentNativeRoomGameplay();
        if(game && game->active() && !game->halted()){
            struct Audit {std::optional<uint64_t> first;unsigned phase=0;bool names=true;};
            static thread_local Audit audit;
            const auto tick=game->committed_tick();
            if(!audit.first){audit.first=tick;audit.names=REXCVAR_GET(em2_online_name_tags);}
            const auto elapsed=tick>=*audit.first?tick-*audit.first:0;
            constexpr std::array<uint64_t,4> boundaries{60,90,120,150};
            if(audit.phase<boundaries.size() && elapsed>=boundaries[audit.phase]){
                ++audit.phase;
                const bool enabled=(audit.phase%2)==0;
                REXCVAR_SET(em2_online_clothing_bind,enabled);
                REXCVAR_SET(em2_online_name_tags,audit.phase==2?false:audit.names);
                REXLOG_INFO("Native Room clothing lifecycle phase: {} tick {} bind {} names {}; isolated render-owner cvar change, no gameplay input",audit.phase,tick,enabled,REXCVAR_GET(em2_online_name_tags));
            }
        }
    }
    if(color_directory.empty() && !color_failed)return;
    const auto directory=SourceKey();
    const bool retire=!available || !Enabled() || !REXCVAR_GET(em2_online_clothing_bind) ||
        (!color_directory.empty() && color_directory!=directory);
    // No native work or allocations on ordinary launches with no cached state.
    if(retire && (!color_directory.empty() || color_failed)){
        if(!ReleaseOnlineClothingDrawResources(ctx,base))color_failed=true;
    }
}
void RegisterOnlineClothingRoot(uint64_t peer,uint32_t entity,uint32_t root,uint32_t rgba,unsigned character){
    if(!Enabled() || !peer || peer>16 || roots.size()>=16 || !entity || !root)return;
    if(std::any_of(roots.begin(),roots.end(),[&](const auto& o){return o.peer==peer || o.entity==entity || o.root==root;}))return;
    roots.push_back({peer,entity,root,rgba,character});
}
bool ReleaseOnlineClothingDrawResources(PPCContext& ctx,uint8_t* base){
    if(color_draw_depth)return false;bool success=true;unsigned released=0;
    for(auto& texture:colors)if(texture.root){std::string error;if(ReleaseNativeClothingTexture(ctx,base,texture,error))++released;else{success=false;REXLOG_ERROR("Native Room clothing cache release failed: {}",error);}}
    if(released)REXLOG_INFO("Native Room clothing cache released: {} textures success {}; native owned references only",released,success);
    if(success){color_directory.clear();color_failed=false;color_reported.clear();roots.clear();seen.clear();}return success;
}
void RenderOnlineClothing(PPCContext& ctx,uint8_t* base,void(*draw)(PPCContext&,uint8_t*)){
    if(!Enabled() || !REXCVAR_GET(em2_online_clothing_bind) || color_failed || roots.empty() || color_draw_depth){draw(ctx,base);return;}
    struct DrawEnvironment {unsigned fp=_mm_getcsr(),input_fp=fp;DWORD error=GetLastError(),input_error=error;
        void Before(){_mm_setcsr(input_fp);SetLastError(input_error);}void After(){fp=_mm_getcsr();error=GetLastError();}
        ~DrawEnvironment(){_mm_setcsr(fp);SetLastError(error);}} environment;
    auto invoke=[&]{environment.Before();try{draw(ctx,base);}catch(...){environment.After();throw;}environment.After();};
    const auto mesh=ctx.r4.u32;std::optional<Owner> owner;NativeClothingBinding binding;
    bool eligible=false;{GuestReadBatch reads;owner=Find(mesh,base);eligible=owner && owner->character<=1 && ReadNativeClothingBinding(mesh,base,binding);}
    if(!eligible){invoke();return;}
    const auto found=std::find_if(kLobbyColors.begin(),kLobbyColors.end(),[&](const auto& c){return c.rgba==owner->rgba;});
    if(found==kLobbyColors.end()){invoke();return;}const auto color=unsigned(found-kLobbyColors.begin());
    if(color==owner->character){invoke();return;}
    const auto directory=REXCVAR_GET(em2_probe_online_clothing_directory);
    const auto source_key=SourceKey();
    if(source_key.empty()){invoke();return;}
    // Directory changes are retired at the frame boundary, before collecting
    // roots. Do not discard the current frame's roots from inside a mesh draw.
    if(!color_directory.empty() && color_directory!=source_key){invoke();return;}
    color_directory=source_key;auto& texture=colors[owner->character*16+color];
    if(!texture.root){
        std::vector<uint8_t> bytes;std::string error;
        if(directory.empty()){
            ClothingTexture prepared;
            if(!ReadMountedOnlineClothingVariant(LobbyCharacter(owner->character),uint8_t(color),prepared,error)){color_failed=true;REXLOG_ERROR("Native Room clothing mounted color failed: {}",error);invoke();return;}
            bytes=std::move(prepared.bytes);
        }else{
            const auto file=std::filesystem::path(std::u8string(directory.begin(),directory.end()))/(std::string(owner->character?"oswald-":"mickey-")+std::to_string(color)+".nif");
            std::error_code ec;const auto length=std::filesystem::file_size(file,ec);
            if(ec || !length || length>2*1024*1024){color_failed=true;REXLOG_ERROR("Native Room clothing color file unavailable");invoke();return;}
            bytes.resize(size_t(length));std::ifstream in(file,std::ios::binary);in.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
            if(!in){color_failed=true;REXLOG_ERROR("Native Room clothing color file read failed");invoke();return;}
        }
        if(!LoadNativeClothingTexture(ctx,base,bytes,texture,error)){color_failed=true;REXLOG_ERROR("Native Room clothing color load failed: {}",error);invoke();return;}
        REXLOG_INFO("Native Room clothing color cached: character {} color {} texture {:08X} renderer {:08X}; native upload once, reused draws",owner->character,color,texture.root,texture.renderer_data);
        {GuestReadBatch reads;REXLOG_INFO("Native Room clothing expected GPU sources: character {} color {} selected {:08X} physical {:08X} original {:08X} normal {:08X} specular {:08X} renderer type {:08X}; native texture header metadata only",owner->character,color,texture.root,Physical(texture.root,base),Physical(binding.textures[0],base),Physical(binding.textures[1],base),Physical(binding.textures[2],base),Readable(texture.renderer_data,4,base)?PPC_LOAD_U32(texture.renderer_data):0);}
    }
    uint32_t held=0;const auto original=binding.textures[0];
    if(!RetainNativeClothingReference(ctx,base,original,held) || !AssignNativeClothingPointer(ctx,base,binding.maps[0]+8,texture.root)){
        DropNativeClothingReference(ctx,base,held);color_failed=true;REXLOG_ERROR("Native Room clothing reference assignment failed; original draw retained");invoke();return;
    }
    ++color_draw_depth;
    auto restore=[&]{
        bool same=false;
        {GuestReadBatch reads;NativeClothingBinding current;same=ReadNativeClothingBinding(mesh,base,current) && current.property==binding.property && current.maps==binding.maps && current.textures[0]==texture.root && current.textures[1]==binding.textures[1] && current.textures[2]==binding.textures[2];}
        const bool restored=same && AssignNativeClothingPointer(ctx,base,binding.maps[0]+8,original);
        const bool dropped=DropNativeClothingReference(ctx,base,held);--color_draw_depth;
        if(!restored || !dropped){color_failed=true;REXLOG_ERROR("Native Room clothing draw restoration failed: restored {} reference dropped {}; color candidate disabled",restored,dropped);}
        else {const auto key=std::tuple(owner->peer,mesh,texture.root);if(std::find(color_reported.begin(),color_reported.end(),key)==color_reported.end() && color_reported.size()<512){color_reported.push_back(key);
            REXLOG_INFO("Native Room clothing colored draw: peer {} character {} color {} mesh {:08X} map {:08X} original {:08X} selected {:08X} restored true normal {:08X} specular {:08X}; original draw and native NiPointer assignment",owner->peer,owner->character,color,mesh,binding.maps[0],original,texture.root,binding.textures[1],binding.textures[2]);}}
    };
    try{invoke();}catch(...){const auto failure=std::current_exception();try{restore();}catch(...){}std::rethrow_exception(failure);}
    restore();
}
void TraceOnlineClothingDraw(PPCContext& ctx,uint32_t mesh,uint8_t* base){
    if(!Enabled() || roots.empty())return;HostState preserve;GuestReadBatch reads;
    if(!Readable(mesh,352,base))return;const auto owner=Find(mesh,base);if(!owner)return;
    {GuestReadBatch::CallbackBoundary boundary;ProbeNativeClothingTextures(ctx,base);}
    const auto key=std::tuple(mesh,owner->root,owner->entity);if(std::find(seen.begin(),seen.end(),key)!=seen.end() || seen.size()>=512)return;seen.push_back(key);
    REXLOG_INFO("Native Room clothing draw owner: peer {} entity {:08X} root {:08X} mesh {:08X} color {:08X} vtable {:08X}; exact registered root ancestry, original draw unchanged",owner->peer,owner->entity,owner->root,mesh,owner->rgba,PPC_LOAD_U32(mesh));
    NativeClothingBinding binding;unsigned stage=0;
    if(!ReadNativeClothingBinding(mesh,base,binding,&stage)){
        REXLOG_INFO("Native Room clothing shader-map route unavailable: peer {} mesh {:08X} stage {}; original draw unchanged",owner->peer,mesh,stage);return;
    }
    REXLOG_INFO("Native Room clothing shader-map binding: peer {} root {:08X} mesh {:08X} state {:08X} property {:08X} array {:08X} maps {:08X}/{:08X}/{:08X} textures {:08X}/{:08X}/{:08X}; exact native diffuse/normal/specular slots, no binding changes",owner->peer,owner->root,mesh,binding.state,binding.property,binding.shader_array,binding.maps[0],binding.maps[1],binding.maps[2],binding.textures[0],binding.textures[1],binding.textures[2]);
}
bool TestOnlineClothingDrawOwnership(){
    HostState preserve;const auto old=roots;struct Restore{std::vector<Owner> old;~Restore(){roots=std::move(old);}}restore{old};
    std::vector<uint8_t> memory(0x20000);auto* base=memory.data();bool ok=true;unsigned count=0;auto check=[&](bool v,const char* text){++count;if(!v){ok=false;REXLOG_ERROR("Online clothing draw ownership: {}",text);}};
    roots={{1,0x4000,0x5000,0xEE4545FF},{3,0x6000,0x7000,0x43BC68FF}};
    PPC_STORE_U32(0x8038,0x7000);PPC_STORE_U32(0x9038,0x5000);
    auto a=Find(0x8000,base),b=Find(0x9000,base);check(a && a->peer==3 && a->rgba==0x43BC68FF,"extra mesh finds its own root/color");check(b && b->peer==1 && b->rgba==0xEE4545FF,"retail mesh keeps different root/color");
    PPC_STORE_U32(0x8038,0x8000);check(!Find(0x8000,base),"parent self-cycle rejected");PPC_STORE_U32(0x8038,0xA000);PPC_STORE_U32(0xA038,0x8000);check(!Find(0x8000,base),"parent multi-cycle rejected");
    roots.clear();check(!Find(0x9000,base),"retired root cannot be reused");
    PPC_STORE_U32(0x8000,0xB000);PPC_STORE_U32(0xB008,0x8259E620);check(Type(0x8000,base)==0x8259E620,"exact native RTTI getter identity");check(!Type(0,base),"null objects rejected");
    PPC_STORE_U32(0x8000+240,0xC000);PPC_STORE_U32(0xC000+40,0xD000);PPC_STORE_U32(0xD000,0xB000);PPC_STORE_U32(0xD000+48,0xE000);
    PPC_STORE_U32(0xE000,0x821D4FD8);PPC_STORE_U32(0xE004,0xF000);PPC_STORE_U16(0xE008,3);PPC_STORE_U16(0xE00A,3);PPC_STORE_U16(0xE00C,3);
    PPC_STORE_U32(0x18008,0x82585D00);
    for(unsigned i=0;i<3;++i){const auto map=0x11000+i*0x1000,texture=0x15000+i*0x1000;PPC_STORE_U32(0xF000+i*4,map);PPC_STORE_U32(map,0x8204373C);PPC_STORE_U32(map+16,0);PPC_STORE_U32(map+8,texture);PPC_STORE_U32(texture,0x18000);}
    NativeClothingBinding binding;check(ReadNativeClothingBinding(0x8000,base,binding) && binding.property==0xD000 && binding.maps[0]==0x11000 && binding.textures==std::array<uint32_t,3>{0x15000,0x16000,0x17000},"native diffuse/normal/specular route");
    const auto retained=binding;PPC_STORE_U32(0x12000+16,1);check(!ReadNativeClothingBinding(0x8000,base,binding) && binding.maps==retained.maps && binding.textures==retained.textures,"unrecognized authored map field rejected without publishing partial route");PPC_STORE_U32(0x12000+16,0);
    PPC_STORE_U32(0x12000+8,0x15000);check(!ReadNativeClothingBinding(0x8000,base,binding),"normal/diffuse texture alias rejected");PPC_STORE_U32(0x12000+8,0x16000);
    PPC_STORE_U16(0xE00A,33);check(!ReadNativeClothingBinding(0x8000,base,binding),"oversized native shader array rejected");PPC_STORE_U16(0xE00A,3);
    PPC_STORE_U32(0xD000,0x18000);check(!ReadNativeClothingBinding(0x8000,base,binding),"source texture cannot masquerade as texturing property");
    if(ok)REXLOG_INFO("Online clothing draw ownership passed {} ancestry/color/alias/retirement/RTTI checks; live material path separate",count);return ok;
}
}
