#include "online_chat.h"
#include "online_room.h"
#include "online_lobby.h"
#include "online_native_menu.h"
#include "online_name_input.h"
#include "online_room_runtime.h"
#include "controls.h"
#include "engine_workbench.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <array>
#include <algorithm>
#include <windows.h>
#include <cstring>
#include <rex/input/input_system.h>
#include <rex/input/input_driver.h>

PPC_EXTERN_FUNC(__imp__sub_8295FC58);
REXCVAR_DEFINE_BOOL(em2_online_text_chat,true,"Online","Native HUD text chat: T or D-pad Up opens; Enter/Y sends; Escape/B closes");
REXCVAR_DEFINE_BOOL(em2_probe_online_chat,false,"Debug","Exact private native-menu Room only: send one chat message per peer and record authoritative receipts");
namespace em2::net {
namespace {
std::mutex mutex;
std::deque<RoomChatMessage> history;
std::deque<std::string> outgoing;
std::atomic<bool> connected{false},opened{false};
LobbyNameInput input(240,240);
std::array<bool,256> held{};
bool opening_character=false;
uint16_t pad_held=0;unsigned keyboard_index=0;
constexpr std::string_view keyboard="abcdefghijklmnopqrstuvwxyz0123456789 .,!?'";
std::string status;
uint32_t revision=0,draft_revision=0;
std::vector<RoomChatMessage> readback;
RoomChatMessage row;
std::chrono::steady_clock::time_point last_message{};
Room* owner=nullptr;unsigned probe_stage=0,expected_peers=0;
bool probe_editor_seen=false;
std::chrono::steady_clock::time_point probe_opened{},probe_submitted{};
void OpenLocked(bool value){opened.store(value);em2::SetChatControlCapture(value);if(value){input.Begin("");em2::ClearControlKeys();}else input.End();status.clear();++draft_revision;}
bool CanOpen(){
    if(!connected.load() || !REXCVAR_GET(em2_online_text_chat))return false;
    int binding=0,panel=0;em2::ControlOption(2200,-1,binding);em2::EngineWorkbenchOption(2500,-1,panel);
    return !binding && !panel && !em2::ControlBindingsOpen();
}
void SendLocked(){
    std::string text=input.text();const auto first=text.find_first_not_of(' '),last=text.find_last_not_of(' ');
    if(first!=std::string::npos)text=text.substr(first,last-first+1);
    if(!ValidRoomChatText(text))status="Enter a message of up to 240 UTF-8 bytes.";
    else if(outgoing.size()==8)status="Chat is busy. Wait a moment and try again.";
    else {outgoing.push_back(std::move(text));input.Begin("");status.clear();}
    ++draft_revision;
}
int Byte(std::string_view text,int index){return index>=0 && size_t(index)<text.size()?uint8_t(text[size_t(index)]):0;}
}
void CloseOnlineChat(){std::lock_guard lock(mutex);OpenLocked(false);held.fill(false);pad_held=0;opening_character=false;}
bool OnlineChatKey(unsigned key,bool down,bool shift,bool control){
    std::lock_guard lock(mutex);
    if(key=='T' && !down)opening_character=false;
    if(key<held.size() && held[key]){if(!down)held[key]=false;if(!down || !opened.load())return true;}
    if(!opened.load()){
        if(key!='T' || !down || !CanOpen())return false;
        held[key]=true;OpenLocked(true);opening_character=true;return true;
    }
    if(down){
        if(key<held.size())held[key]=true;
        if(key==VK_ESCAPE)OpenLocked(false);
        else if(key==VK_RETURN)SendLocked();
        else if(control && key=='V'){
            if(OpenClipboard(nullptr)){
                if(auto data=GetClipboardData(CF_UNICODETEXT)){
                    const auto bytes=GlobalSize(data);
                    if(bytes && bytes<=8192)if(const auto text=static_cast<const wchar_t*>(GlobalLock(data))){
                        for(size_t i=0;i<bytes/sizeof(wchar_t) && text[i];++i)input.Character(uint16_t(text[i]));
                        GlobalUnlock(data);
                    }
                }CloseClipboard();
            }++draft_revision;
        }else {input.Key(key,shift,control);++draft_revision;}
    }
    return true;
}
bool OnlineChatCharacter(uint32_t unit){std::lock_guard lock(mutex);if(!opened.load())return false;if(opening_character){opening_character=false;if(unit=='t' || unit=='T')return true;}input.Character(unit);++draft_revision;return true;}
bool OnlineChatPad(uint16_t buttons){
    std::lock_guard lock(mutex);const auto edge=uint16_t(buttons&~pad_held);pad_held=buttons;
    if(!opened.load()){if((edge&1) && CanOpen())OpenLocked(true);return opened.load();}
    if(edge&0x2000){OpenLocked(false);return true;}
    if(edge&1)keyboard_index=unsigned((keyboard_index+keyboard.size()-10)%keyboard.size());
    if(edge&2)keyboard_index=unsigned((keyboard_index+10)%keyboard.size());
    if(edge&4)keyboard_index=unsigned((keyboard_index+keyboard.size()-1)%keyboard.size());
    if(edge&8)keyboard_index=unsigned((keyboard_index+1)%keyboard.size());
    if(edge&0x1000)input.Character(uint8_t(keyboard[keyboard_index]));
    if(edge&0x4000)input.Key(VK_BACK,false,false);
    if(edge&0x8000)SendLocked();
    if(edge)++draft_revision;
    return true;
}
void PumpOnlineChat(Room* room){
    if(owner!=room){std::lock_guard lock(mutex);history.clear();outgoing.clear();owner=room;probe_stage=0;expected_peers=0;probe_editor_seen=false;++revision;}
    const bool active=room && room->state()==RoomState::Running;connected.store(active);
    if(!active && opened.load())CloseOnlineChat();
    if(!room)return;
    const bool private_probe=active && REXCVAR_GET(em2_probe_online_chat) && PrivateNativeMenuRoom()==room;
    if(private_probe){
        {std::lock_guard lock(mutex);expected_peers=unsigned(room->lobby().players.size());}
        const auto tick=room->next_tick();
        if(probe_stage==0 && tick>=430){OnlineChatKey('T',true);OnlineChatCharacter('t');OnlineChatKey('T',false);probe_opened=std::chrono::steady_clock::now();++probe_stage;}
        else if(probe_stage==1 && tick>=431){for(auto c:"Private chat peer "+std::to_string(room->local_peer()))OnlineChatCharacter(uint8_t(c));++probe_stage;}
        else if(probe_stage==2){
            bool seen;{std::lock_guard lock(mutex);seen=probe_editor_seen;}
            if(seen || std::chrono::steady_clock::now()-probe_opened>std::chrono::seconds(2)){OnlineChatKey(VK_RETURN,true);OnlineChatKey(VK_RETURN,false);probe_submitted=std::chrono::steady_clock::now();++probe_stage;}
        }else if(probe_stage==3 && std::chrono::steady_clock::now()-probe_submitted>std::chrono::milliseconds(400)){OnlineChatKey(VK_ESCAPE,true);OnlineChatKey(VK_ESCAPE,false);++probe_stage;}
    }
    std::deque<std::string> pending;
    {std::lock_guard lock(mutex);if(active)pending.swap(outgoing);}
    for(const auto& text:pending){
        if(!room->SendChat(text)){std::lock_guard lock(mutex);status="Chat is busy. Wait a moment and try again.";++draft_revision;}
        else if(private_probe)REXLOG_INFO("Native Room private chat sent: peer {}; owner-thread text channel, no simulation command",room->local_peer());
    }
    auto received=room->TakeChatMessages();
    if(!received.empty()){
        std::lock_guard lock(mutex);last_message=std::chrono::steady_clock::now();++revision;
        for(auto& m:received){
            if(REXCVAR_GET(em2_probe_online_chat) && PrivateNativeMenuRoom()==room)
                REXLOG_INFO("Native Room private chat received: sequence {} peer {} color {} text '{}'; host-authorized identity",m.sequence,m.peer,m.color,m.text);
            if(history.size()==64)history.pop_front();history.push_back(std::move(m));
        }
    }
}
void EnsureNativeChatBridge(PPCContext& ctx,uint8_t* base){
    if(!NativeRoomActive() || ctx.r1.u32<2048)return;
    const auto dispatcher=PPC_LOAD_U32(0x833DCB9C),movie=dispatcher?PPC_LOAD_U32(dispatcher+4):0;
    const auto handle=movie?PPC_LOAD_U32(movie+156):0;if(!handle)return;
    // The HUD shares the native movie dispatcher. A distinct method name
    // avoids borrowing/overwriting OptionsMenuState's SetOption registration.
    static thread_local uint32_t prior_dispatcher=0,prior_movie=0,prior_handle=0;
    static thread_local Room* prior_owner=nullptr;
    Room* current_owner;{std::lock_guard lock(mutex);current_owner=owner;}
    if(prior_dispatcher==dispatcher && prior_movie==movie && prior_handle==handle && prior_owner==current_owner)return;
    struct Preserve{PPCContext& c;PPCContext saved;unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Preserve(){c=saved;_mm_setcsr(csr);SetLastError(error);}}preserve{ctx,ctx};
    const auto stack=ctx.r1.u32-512;PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;
    constexpr char name[]="PCOnlineChat";std::memcpy(base+stack+128,name,sizeof(name));
    ctx.r3.u64=0x82B0D118;ctx.r4.u64=stack+128;ctx.r5.u64=0;ctx.r6.u64=1;
    __imp__sub_8295FC58(ctx,base);
    prior_dispatcher=dispatcher;prior_movie=movie;prior_handle=handle;prior_owner=current_owner;
    REXLOG_INFO("Native HUD chat callback registered on current movie; separate from options callbacks");
}
bool NativeOnlineChatOption(int id,int choice,int& result){
    if(id<2750 || id>2763)return false;
    std::lock_guard lock(mutex);result=0;
    if(id==2750){
        const bool enabled=connected.load() && REXCVAR_GET(em2_online_text_chat);
        if(!enabled && opened.load())OpenLocked(false);
        const bool recent=std::chrono::steady_clock::now()-last_message<std::chrono::seconds(12);
        result=enabled?(1|(opened.load()?2:0)|((recent || opened.load())?4:0)):0;
    }else if(id==2751){readback.assign(history.begin(),history.end());result=int(revision&0x7FFFFFFF);}
    else if(id==2752)result=int(readback.size());
    else if(id==2753){row=choice>=0 && size_t(choice)<readback.size()?readback[size_t(choice)]:RoomChatMessage{};result=row.peer?1:0;}
    else if(id==2754)result=Byte(row.name,choice);
    else if(id==2755)result=Byte(row.text,choice);
    else if(id==2756)result=int(kLobbyColors[row.color].rgba>>8);
    else if(id==2757)result=Byte(input.text(),choice);
    else if(id==2758)result=choice==0?int(input.cursor_utf16()):(choice==1?int(input.anchor_utf16()):int(draft_revision&0x7FFFFFFF));
    else if(id==2759){if(choice==0 && CanOpen())OpenLocked(true);else if(choice==1 && opened.load())SendLocked();else if(choice==2)OpenLocked(false);result=opened.load()?1:0;}
    else if(id==2760)result=Byte(status,choice);
    else if(id==2762){std::string grid;for(size_t i=0;i<keyboard.size();++i){if(i && i%10==0)grid+='\n';grid+=i==keyboard_index?'[':' ';grid+=keyboard[i]==' '?char('_'):keyboard[i];grid+=i==keyboard_index?']':' ';}result=Byte(grid,choice);}
    else if(id==2763)result=int(keyboard_index);
    else if(id==2761){
        static thread_local Room* seen=nullptr;static thread_local unsigned seen_stages=0;
        if(seen!=owner){seen=owner;seen_stages=0;}
        if(owner && choice>=1 && choice<=3 && !(seen_stages&(1u<<choice))){
            if(choice==1)REXLOG_INFO("Native HUD chat page initialized by authored GFx; no ImGui panel");
            else if(REXCVAR_GET(em2_probe_online_chat) && PrivateNativeMenuRoom()==owner){
                if(choice==2 && opened.load() && input.text().starts_with("Private chat peer ")){probe_editor_seen=true;REXLOG_INFO("Native HUD chat editor readback accepted: {} UTF-8 bytes, cursor {}; authored draft field, gameplay capture {}",input.text().size(),input.cursor_utf16(),em2::ControlBindingsOpen());}
                else if(choice==3 && expected_peers && readback.size()==expected_peers)REXLOG_INFO("Native HUD chat history readback accepted: {} messages; authored colored name and plain message fields",readback.size());
                else return true;
            }
            seen_stages|=1u<<choice;
        }
    }
    return true;
}
bool TestOnlineChatControls(){
    using rex::X_STATUS;using rex::X_RESULT;
    if(connected.load() || opened.load() || em2::ControlBindingsOpen())return false;
    const bool enabled=REXCVAR_GET(em2_online_text_chat);REXCVAR_SET(em2_online_text_chat,true);
    bool ok=!OnlineChatKey('T',true);OnlineChatKey('T',false);connected.store(true);
    ok=OnlineChatKey('T',true) && opened.load() && em2::ControlBindingsOpen() && ok;OnlineChatKey('T',false);
    OnlineChatCharacter('x');OnlineChatCharacter(0x1F600);
    {std::lock_guard lock(mutex);ok=input.text()=="x\xF0\x9F\x98\x80" && ok;}
    ok=OnlineChatKey('W',true) && ok;em2::SetExternalControlCapture(true);CloseOnlineChat();
    ok=!opened.load() && em2::ControlBindingsOpen() && ok;em2::SetExternalControlCapture(false);
    ok=!em2::ControlBindingsOpen() && ok;
    ok=OnlineChatKey('T',true) && OnlineChatKey(VK_ESCAPE,true) && !opened.load() && !em2::ControlBindingsOpen() && ok;
    OnlineChatKey(VK_ESCAPE,false);OnlineChatKey('T',false);
    OnlineChatPad(1);ok=opened.load() && em2::ControlBindingsOpen() && ok;OnlineChatPad(0);
    OnlineChatPad(0x1000);OnlineChatPad(0);OnlineChatPad(0x8000);OnlineChatPad(0);
    {std::lock_guard lock(mutex);ok=outgoing.size()==1 && outgoing.front()=="a" && input.text().empty() && ok;outgoing.clear();}
    OnlineChatPad(0x2000);ok=!opened.load() && !em2::ControlBindingsOpen() && ok;OnlineChatPad(0);
    // Closing chat while moving must resume analog motion immediately, while
    // its A/B buttons remain blocked until release. Exercise actual sampling.
    class Pad final:public rex::input::InputDriver {
    public: rex::input::X_INPUT_GAMEPAD raw{};
        Pad():InputDriver(nullptr,0){}
        rex::X_STATUS Setup()override{return X_STATUS_SUCCESS;}
        rex::X_RESULT GetCapabilities(uint32_t,uint32_t,rex::input::X_INPUT_CAPABILITIES*)override{return X_ERROR_DEVICE_NOT_CONNECTED;}
        rex::X_RESULT GetState(uint32_t user,rex::input::X_INPUT_STATE* out)override{if(user || !out)return X_ERROR_DEVICE_NOT_CONNECTED;*out={};out->gamepad=raw;return X_ERROR_SUCCESS;}
        rex::X_RESULT SetState(uint32_t,rex::input::X_INPUT_VIBRATION*)override{return X_ERROR_DEVICE_NOT_CONNECTED;}
        rex::X_RESULT GetKeystroke(uint32_t,uint32_t,rex::input::X_INPUT_KEYSTROKE*)override{return X_ERROR_EMPTY;}
    };
    rex::input::InputSystem source(nullptr);auto driver=std::make_unique<Pad>();auto* physical=driver.get();source.AddDriver(std::move(driver));
    if(em2::BeginSynchronizedControls(0)){
        em2::net::Pad sampled{};ok=em2::SampleLocalControls(source,0,0,sampled) && ok;
        OnlineChatKey('T',true);OnlineChatKey('T',false);physical->raw.buttons=0x3000;physical->raw.thumb_ly=16000;
        ok=em2::SampleLocalControls(source,0,1,sampled) && sampled.left_y>0 && !(sampled.buttons&0x3000) && !opened.load() && ok;
        ok=em2::SampleLocalControls(source,0,2,sampled) && sampled.left_y>0 && !(sampled.buttons&0x3000) && ok;
        physical->raw.buttons=0;ok=em2::SampleLocalControls(source,0,3,sampled) && ok;
        physical->raw.buttons=0x1000;ok=em2::SampleLocalControls(source,0,4,sampled) && (sampled.buttons&0x1000) && ok;
        em2::EndSynchronizedControls();
    }else ok=false;
    CloseOnlineChat();connected.store(false);REXCVAR_SET(em2_online_text_chat,enabled);
    if(ok)REXLOG_INFO("Chat controls passed offline gating, T/open, typing capture, independent console capture, Escape release and controller keyboard/send/close; visual typing acceptance separate");
    else REXLOG_ERROR("Chat control ownership regression failed");return ok;
}
}
