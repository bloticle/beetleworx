#include "online_nametags.h"
#include "online_room_gameplay.h"
#include "online_viewport.h"
#include "online_lobby.h"
#include "online_clothing_draw.h"
#include "online_voice_runtime.h"
#include "allocation_trace.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <imgui.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <mutex>
#include <windows.h>
#include <immintrin.h>

REXCVAR_DEFINE_BOOL(em2_online_name_tags,true,"Online","Show other online players' names in their unique lobby colors");
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_room_probe_scripted_input);
namespace em2::net {
namespace {
using Clock=std::chrono::steady_clock;
struct Label {uint64_t peer;float x,y;uint32_t rgba;std::string name;};
std::mutex label_mutex;
std::vector<Label> labels;
Clock::time_point published{};
struct HostState {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~HostState(){_mm_setcsr(fp);SetLastError(error);}};
bool Readable(uint32_t at,size_t count,uint8_t* base){return base && at>=4096 && uint64_t(at)+count<=0x100000000ull && GuestReadBatch::Readable(base+at,count);}
float Float(uint32_t at,uint8_t* base){return std::bit_cast<float>(PPC_LOAD_U32(at));}
uint32_t Graph(uint32_t entity,uint8_t* base){
    if(!Readable(entity,96,base) || PPC_LOAD_U32(entity)!=0x8208D908 || !Readable(0x833DB0EC,1,base))return 0;
    const auto kind=PPC_LOAD_U8(0x833DB0EC);
    const auto types=PPC_LOAD_U32(entity+88),components=PPC_LOAD_U32(entity+36),count=PPC_LOAD_U32(entity+40);
    if(!Readable(types,unsigned(kind)+1,base))return 0;
    const auto index=PPC_LOAD_U8(types+kind);
    if(index==255 || index>=count || !Readable(components+unsigned(index)*4,4,base))return 0;
    const auto face=PPC_LOAD_U32(components+unsigned(index)*4);
    if(face<8 || !Readable(face-8,92,base) || PPC_LOAD_U32(face-8+48)!=entity-8)return 0;
    const auto node=PPC_LOAD_U32(face-8+88);
    return Readable(node,208,base)?node:0;
}
bool Camera(uint32_t node,uint8_t* base,NameTagCamera& out){
    if(!Readable(node,368,base) || PPC_LOAD_U32(node)!=0x8203E610)return false;
    for(unsigned i=0;i<3;++i){out.right[i]=Float(node+144+i*4,base);out.up[i]=Float(node+160+i*4,base);out.forward[i]=Float(node+176+i*4,base);out.position[i]=Float(node+192+i*4,base);}
    out.left=Float(node+304,base);out.right_edge=Float(node+308,base);out.top=Float(node+312,base);out.bottom=Float(node+316,base);
    out.near_plane=Float(node+320,base);out.far_plane=Float(node+324,base);out.orthographic=PPC_LOAD_U8(node+328)!=0;
    return true;
}
}
bool ProjectNameTag(const NameTagCamera& c,const std::array<float,3>& p,float& x,float& y){
    double right=0,up=0,depth=0;
    for(unsigned i=0;i<3;++i){
        if(!std::isfinite(p[i]) || !std::isfinite(c.position[i]) || !std::isfinite(c.right[i]) || !std::isfinite(c.up[i]) || !std::isfinite(c.forward[i]))return false;
        const double d=double(p[i])-c.position[i];right+=d*c.right[i];up+=d*c.up[i];depth+=d*c.forward[i];
    }
    for(float v:{c.left,c.right_edge,c.top,c.bottom,c.near_plane,c.far_plane})if(!std::isfinite(v))return false;
    if(c.near_plane<=0 || c.far_plane<=c.near_plane || depth<c.near_plane || depth>c.far_plane || c.right_edge<=c.left || c.top<=c.bottom)return false;
    if(!c.orthographic){right/=depth;up/=depth;}
    const double u=(right-c.left)/(double(c.right_edge)-c.left),v=(c.top-up)/(double(c.top)-c.bottom);
    if(!std::isfinite(u) || !std::isfinite(v) || u<0 || u>1 || v<0 || v>1)return false;
    x=float(u);y=float(v);return true;
}
void ClearOnlineNameTags(){ClearOnlineVoiceScene();std::lock_guard lock(label_mutex);labels.clear();published={};}
void CaptureOnlineNameTags(PPCContext& ctx,uint8_t* base){
    HostState preserve;GuestReadBatch reads;
    std::vector<OnlineNameActor> actors;uint64_t local=0;
    auto* game=CurrentNativeRoomGameplay();
    NameTagCamera camera;
    OnlineVoiceScene voice;
    const bool available=game && game->NameTagActors(actors,local,base,&voice.session);
    {GuestReadBatch::CallbackBoundary boundary;BeginOnlineClothingDrawFrame(ctx,base,available);}
    if(!available){ClearOnlineNameTags();return;}
    const bool camera_valid=Camera(NativeRoomViewCamera(base),base,camera);
    const bool show_names=REXCVAR_GET(em2_online_name_tags) && camera_valid;
    voice.world=em2::CurrentNativeLoadGeneration();voice.local_peer=local;voice.right=camera.right;
    std::vector<Label> next;next.reserve(actors.size());
    thread_local uint64_t reported_local=0;thread_local uint16_t reported=0;
    if(reported_local!=local){reported_local=local;reported=0;}
    for(const auto& actor:actors){
        const auto node=Graph(actor.entity,base);if(!node)continue;
        RegisterOnlineClothingRoot(actor.peer,actor.entity,node,actor.rgba,actor.character);
        std::array<float,3> anchor;for(unsigned i=0;i<3;++i)anchor[i]=Float(node+192+i*4,base);
        if(actor.peer>=1 && actor.peer<=16){voice.players[actor.peer-1]={true,voice.world,anchor};if(actor.peer==local)voice.listener=voice.players[actor.peer-1];}
        if(!show_names || actor.peer==local)continue;
        anchor[1]+=2.1f;float x,y;if(!ProjectNameTag(camera,anchor,x,y))continue;
        next.push_back({actor.peer,x,y,actor.rgba,actor.name});
        if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_room_probe_scripted_input) && actor.peer>=1 && actor.peer<=16 && !(reported&(1u<<(actor.peer-1)))){
            reported|=uint16_t(1u<<(actor.peer-1));
            REXLOG_INFO("Native Room name tag: local peer {} target peer {} color {:08X} projected {}/{} name '{}'; rendered native actor/camera pose, host-only UI snapshot",local,actor.peer,actor.rgba,x,y,actor.name);
        }
    }
    if(camera_valid && PublishOnlineVoiceScene(voice)){
        thread_local uint64_t voice_generation=0,voice_local=0;thread_local unsigned voice_mask=0;
        unsigned mask=0;for(unsigned i=0;i<16;++i)if(voice.players[i].valid)mask|=1u<<i;
        if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_room_probe_scripted_input) && (voice_generation!=voice.world || voice_local!=local || voice_mask!=mask)){
            voice_generation=voice.world;voice_local=local;voice_mask=mask;
            REXLOG_INFO("Native Room voice scene: local peer {} generation {} mask {:04X} listener {}/{}/{} right {}/{}/{}; copied native actor/camera values, no audio-thread guest reads",local,voice.world,mask,voice.listener.point[0],voice.listener.point[1],voice.listener.point[2],voice.right[0],voice.right[1],voice.right[2]);
        }
    }else ClearOnlineVoiceScene();
    std::lock_guard lock(label_mutex);labels=std::move(next);published=Clock::now();
}
void DrawOnlineNameTags(float width,float height){
    HostState preserve;
    if(!REXCVAR_GET(em2_online_name_tags) || !std::isfinite(width) || !std::isfinite(height) || width<160 || height<120)return;
    std::vector<Label> current;
    {std::lock_guard lock(label_mutex);if(Clock::now()-published>std::chrono::milliseconds(250))return;current=labels;}
    auto* draw=ImGui::GetForegroundDrawList();
    for(const auto& label:current){
        const auto size=ImGui::CalcTextSize(label.name.c_str());const float x=label.x*width-size.x*.5f,y=label.y*height-size.y;
        const auto rgba=label.rgba;const auto color=IM_COL32(rgba>>24,(rgba>>16)&255,(rgba>>8)&255,rgba&255);
        draw->AddRectFilled(ImVec2(x-5,y-3),ImVec2(x+size.x+5,y+size.y+3),IM_COL32(0,0,0,155),3);
        draw->AddText(ImVec2(x+1,y+1),IM_COL32(0,0,0,255),label.name.c_str());draw->AddText(ImVec2(x,y),color,label.name.c_str());
    }
}
bool TestOnlineNameTags(){
    HostState preserve;bool ok=true;unsigned count=0;auto check=[&](bool value,const char* why){++count;if(!value){ok=false;REXLOG_ERROR("Online name-tag projection: {}",why);}};
    NameTagCamera c;c.right={1,0,0};c.up={0,1,0};c.forward={0,0,1};c.left=-1;c.right_edge=1;c.bottom=-1;c.top=1;c.near_plane=.1f;c.far_plane=100;
    float x=-1,y=-1;check(ProjectNameTag(c,{0,0,5},x,y) && x==.5f && y==.5f,"center projects to center");
    check(ProjectNameTag(c,{2,1,4},x,y) && x==.75f && y==.375f,"perspective axes and Y orientation");
    check(!ProjectNameTag(c,{0,0,-1},x,y),"behind-camera label hidden");check(!ProjectNameTag(c,{0,0,.05f},x,y),"near clipping");check(!ProjectNameTag(c,{0,0,101},x,y),"far clipping");
    check(!ProjectNameTag(c,{6,0,5},x,y),"outside-frustum label hidden");
    c.orthographic=true;check(ProjectNameTag(c,{.5f,.25f,5},x,y) && x==.75f && y==.375f,"orthographic uses authored frustum");
    c.position={10,20,30};check(ProjectNameTag(c,{10,20,35},x,y) && x==.5f && y==.5f,"camera translation");
    c.position={};c.orthographic=false;c.right={0,0,-1};c.forward={1,0,0};check(ProjectNameTag(c,{5,0,0},x,y) && x==.5f && y==.5f,"rotated camera basis");
    c.right_edge=c.left;check(!ProjectNameTag(c,{5,0,0},x,y),"degenerate frustum hidden");
    if(ok)REXLOG_INFO("Online name-tag projection passed {} perspective/orthographic/camera/clipping checks; native actor/render capture remains separate",count);return ok;
}
}
