#include "online_discord.h"
#include "demo_support_json.h"
#include <windows.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

REXCVAR_DEFINE_STRING(em2_discord_application_id,"1262682777153962059","Online","Beetleworx Discord Application ID (public identifier, not a token)");
REXCVAR_DEFINE_BOOL(em2_discord_invites,true,"Online","Enable Discord party presence and user-accepted online invitations");
REXCVAR_DEFINE_BOOL(em2_discord_launch,false,"Online","Discord launched this game to accept an invitation; go directly to Epic Mickey 2 setup/title");
REXCVAR_DECLARE(bool,em2_private_profile);
namespace em2::net {
namespace {
using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;
using Bytes=std::vector<uint8_t>;
bool Code(std::string_view value){return value.size()==8 && std::all_of(value.begin(),value.end(),[](char c){return std::string_view("ABCDEFGHJKLMNPQRSTUVWXYZ23456789").find(c)!=std::string_view::npos;});}
void Need(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle=INVALID_HANDLE_VALUE):value(handle){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE && value)CloseHandle(value);}
    Handle(const Handle&)=delete;
};
struct Party {std::string code;unsigned players=1,capacity=16;bool joinable=false;bool operator==(const Party&)const=default;};
class Discord;
std::atomic<Discord*> live_service=nullptr;
class Discord {
    std::mutex mutex;
    std::condition_variable wake;
    Party wanted;
    std::string app,pending,status="Discord is not connected.";
    std::jthread worker;
    uint64_t revision=1;
    uint64_t confirmed_revision=0;
    bool invite_requested=false;
    bool invite_busy=false;
    bool enabled=false;
    void RegisterLaunch(const std::string& application){
        std::array<wchar_t,32768> path{};const auto length=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
        Need(length && length<path.size(),"Cannot locate Beetleworx for Discord launch registration");
        const auto protocol=L"Software\\Classes\\discord-"+std::wstring(application.begin(),application.end());
        HKEY key=nullptr;
        Need(RegCreateKeyExW(HKEY_CURRENT_USER,protocol.c_str(),0,nullptr,0,KEY_SET_VALUE|KEY_CREATE_SUB_KEY,nullptr,&key,nullptr)==ERROR_SUCCESS,"Cannot register Discord game launch for this Windows user");
        struct Registry{HKEY value;~Registry(){RegCloseKey(value);}} registry{key};
        auto set=[&](const wchar_t* subkey,const wchar_t* name,const std::wstring& value){
            Need(RegSetKeyValueW(key,subkey,name,REG_SZ,value.c_str(),DWORD((value.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS,"Cannot save Discord game launch registration");
        };
        set(nullptr,nullptr,L"URL:Beetleworx Discord game invitation");set(nullptr,L"URL Protocol",L"");
        set(L"DefaultIcon",nullptr,L"\""+std::wstring(path.data(),length)+L"\",0");
        // The URL is deliberately not interpolated into the command line.
        // Discord delivers the actual join secret over local Discord RPC.
        set(L"shell\\open\\command",nullptr,L"\""+std::wstring(path.data(),length)+L"\" --em2_discord_launch=true");
    }
    void Status(std::string value){
        std::lock_guard lock(mutex);if(status==value)return;
        status=std::move(value);REXLOG_INFO("Discord integration: {}",status);
    }
    // Every pending operation owns its buffer/event until completion, including
    // cancellation. No synchronous pipe I/O runs on the game's UI thread.
    bool Transfer(HANDLE pipe,void* memory,DWORD size,bool writing,std::stop_token stop){
        Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));Need(bool(event.value),"Cannot create Discord IPC event");
        OVERLAPPED operation{};operation.hEvent=event.value;DWORD transferred=0;
        const auto deadline=Clock::now()+3s;
        DWORD offset=0;
        do{
        ResetEvent(event.value);operation={};operation.hEvent=event.value;transferred=0;
        auto* target=static_cast<uint8_t*>(memory)+offset;
        const auto result=writing?WriteFile(pipe,target,size-offset,&transferred,&operation):ReadFile(pipe,target,size-offset,&transferred,&operation);
        if(!result){
            if(GetLastError()!=ERROR_IO_PENDING)return false;
            for(;;){
                const auto wait=WaitForSingleObject(event.value,50);
                if(wait==WAIT_OBJECT_0)break;
                if(wait!=WAIT_TIMEOUT || stop.stop_requested() || Clock::now()>=deadline){
                    CancelIoEx(pipe,&operation);GetOverlappedResult(pipe,&operation,&transferred,TRUE);return false;
                }
            }
            if(!GetOverlappedResult(pipe,&operation,&transferred,FALSE))return false;
        }
        if(!transferred || stop.stop_requested())return false;
        offset+=transferred;
        if(writing)return offset==size; // A frame is one complete pipe write.
        if(Clock::now()>=deadline && offset<size)return false;
        }while(offset<size);
        return true;
    }
    bool Send(HANDLE pipe,uint32_t opcode,std::string_view body,std::stop_token stop){
        if(body.size()>65536 || stop.stop_requested())return false;
        Bytes bytes(8+body.size());
        for(unsigned i=0;i<4;++i){bytes[i]=uint8_t(opcode>>(i*8));bytes[4+i]=uint8_t(body.size()>>(i*8));}
        std::copy(body.begin(),body.end(),bytes.begin()+8);
        return Transfer(pipe,bytes.data(),DWORD(bytes.size()),true,stop);
    }
    std::string Activity(const Party& party,uint64_t nonce){
        std::string activity="null";
        if(!party.code.empty()){
            activity="{\"type\":0,\"details\":\"Epic Mickey 2 online co-op\",\"state\":"+
                demo_detail::Quote(party.joinable?"In the lobby":"Playing online")+
                ",\"party\":{\"id\":"+demo_detail::Quote("beetleworx:"+party.code)+",\"size\":["+std::to_string(party.players)+","+std::to_string(party.capacity)+"]},\"instance\":true";
            if(party.joinable && party.players<party.capacity)activity+=",\"secrets\":{\"join\":"+demo_detail::Quote("BW1:"+party.code)+"}";
            activity+="}";
        }
        return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"+std::to_string(GetCurrentProcessId())+",\"activity\":"+activity+"},\"nonce\":\"bw-"+std::to_string(nonce)+"\"}";
    }
    void Connected(HANDLE pipe,const std::string& application,std::stop_token stop){
        struct Reset{Discord& owner;~Reset(){std::lock_guard lock(owner.mutex);owner.confirmed_revision=0;owner.invite_requested=false;owner.invite_busy=false;}}reset{*this};
        Need(Send(pipe,0,"{\"v\":1,\"client_id\":"+demo_detail::Quote(application)+"}",stop),"Discord handshake could not be sent");
        bool ready=false;uint64_t sent_revision=0,nonce=0;
        auto handshake_deadline=Clock::now()+10s,last_activity=Clock::now()-5s;
        std::string activity_nonce,invite_nonce;Party sent_party;auto acknowledgement=Clock::now(),invite_deadline=Clock::now();
        while(!stop.stop_requested()){
            Party party;uint64_t current_revision;bool active;
            {std::lock_guard lock(mutex);active=enabled && app==application;party=wanted;current_revision=revision;}
            if(!active)return;
            DWORD available=0;Need(PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr),"Discord disconnected");
            if(available>=8){
                std::array<uint8_t,8> header{};Need(Transfer(pipe,header.data(),DWORD(header.size()),false,stop),"Discord header was interrupted");
                uint32_t opcode=0,length=0;for(unsigned i=0;i<4;++i){opcode|=uint32_t(header[i])<<(8*i);length|=uint32_t(header[4+i])<<(8*i);}
                Need(length<=65536,"Discord frame exceeded its size limit");std::string body(length,'\0');
                Need(!length || Transfer(pipe,body.data(),length,false,stop),"Discord frame was interrupted");
                if(opcode==3){Need(Send(pipe,4,body,stop),"Discord heartbeat failed");continue;}
                Need(opcode==1,"Discord closed the connection");
                auto json=demo_detail::ParseJson(body);const auto* event=json.Find("evt");const auto* command=json.Find("cmd");
                const auto name=event && event->kind==demo_detail::Json::String?event->Str():std::string{};
                const auto* response=json.Find("nonce");
                if(response && response->kind==demo_detail::Json::String && !invite_nonce.empty() && response->Str()==invite_nonce){
                    invite_nonce.clear();
                    {std::lock_guard lock(mutex);invite_busy=false;}
                    Status(name=="ERROR"?"Discord could not open its invite screen. Use Copy room code, or check Discord's game activity settings.":"Discord invite screen opened. Choose the friends or channel to invite.");
                    continue;
                }
                if(name=="ERROR")throw std::runtime_error("Discord rejected the integration request. Check the Application ID and application configuration.");
                if(command && command->kind==demo_detail::Json::String && command->Str()=="DISPATCH"){
                    if(name=="READY"){
                        Need(!ready,"Repeated Discord handshake");ready=true;
                        Need(Send(pipe,1,"{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"nonce\":\"bw-subscribe-join\"}",stop),"Discord join subscription failed");
                        Status("Discord connected. Host a room to enable invites.");
                    }else if(name=="ACTIVITY_JOIN" && ready){
                        auto secret=json.At("data").At("secret").Str();
                        if(secret.starts_with("BW1:") && Code(std::string_view(secret).substr(4))){
                            std::lock_guard lock(mutex);pending=secret.substr(4); // Latest accepted invite wins; no external text reaches the console.
                        }
                    }
                }
                if(response && response->kind==demo_detail::Json::String && !activity_nonce.empty() && response->Str()==activity_nonce){
                    activity_nonce.clear();
                    {std::lock_guard lock(mutex);confirmed_revision=sent_revision;}
                    Status(sent_party.code.empty()?"Discord connected.":sent_party.joinable?
                        "Discord invites ready. Invite friends through Discord.":"Discord connected. This game has already started.");
                }
            }
            Need(ready || Clock::now()<handshake_deadline,"Discord handshake timed out");
            Need(activity_nonce.empty() || Clock::now()-acknowledgement<10s,"Discord activity acknowledgement timed out");
            if(ready && activity_nonce.empty() && sent_revision!=current_revision && Clock::now()-last_activity>=5s){
                ++nonce;activity_nonce="bw-"+std::to_string(nonce);
                Need(Send(pipe,1,Activity(party,nonce),stop),"Discord activity update failed");
                sent_party=party;
                sent_revision=current_revision;last_activity=acknowledgement=Clock::now();
            }
            bool open_invite=false;
            {std::lock_guard lock(mutex);
                if(ready && activity_nonce.empty() && invite_nonce.empty() && invite_requested && confirmed_revision==revision && wanted.joinable && wanted.players<wanted.capacity){invite_requested=false;open_invite=true;}
            }
            if(open_invite){
                invite_nonce="bw-invite-"+std::to_string(++nonce);invite_deadline=Clock::now()+10s;
                // The official Game SDK's OpenActivityInvite(Join) operation,
                // over our existing local IPC connection; no helper or token.
                Need(Send(pipe,1,"{\"cmd\":\"OPEN_OVERLAY_ACTIVITY_INVITE\",\"args\":{\"type\":1,\"pid\":"+std::to_string(GetCurrentProcessId())+"},\"nonce\":"+demo_detail::Quote(invite_nonce)+"}",stop),"Discord invite screen request failed");
            }
            if(!invite_nonce.empty() && Clock::now()>invite_deadline){invite_nonce.clear();{std::lock_guard lock(mutex);invite_busy=false;}Status("Discord did not open its invite screen. Use Copy room code instead.");}
            std::unique_lock lock(mutex);wake.wait_for(lock,50ms);
        }
    }
    void Run(std::stop_token stop){
        std::string registered;
        while(!stop.stop_requested()){
            std::string application;bool active;
            {std::lock_guard lock(mutex);application=app;active=enabled;}
            if(active){
                if(registered!=application){
                    try{RegisterLaunch(application);registered=application;}
                    catch(const std::exception& error){Status(error.what());}
                }
                bool found=false;
                for(unsigned index=0;index<10 && !stop.stop_requested();++index){
                    const auto name=L"\\\\?\\pipe\\discord-ipc-"+std::to_wstring(index);
                    Handle pipe(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
                    if(pipe.value==INVALID_HANDLE_VALUE)continue;
                    found=true;
                    try{Status("Connecting to Discord...");Connected(pipe.value,application,stop);}
                    catch(const std::exception& error){if(!stop.stop_requested())Status(error.what());}
                    break;
                }
                if(!found)Status("Open the Discord desktop app to use game invites.");
            }
            std::unique_lock lock(mutex);wake.wait_for(lock,5s);
        }
    }
public:
    Discord(){live_service.store(this,std::memory_order_release);}
    ~Discord(){live_service.store(nullptr,std::memory_order_release);worker.request_stop();wake.notify_all();if(worker.joinable())worker.join();}
    void Tick(){
        auto id=REXCVAR_GET(em2_discord_application_id);uint64_t number=0;
        const auto parsed=std::from_chars(id.data(),id.data()+id.size(),number);
        const bool valid=id.size()>=17 && id.size()<=20 && parsed.ec==std::errc{} && parsed.ptr==id.data()+id.size() && number && id.front()!='0';
        std::lock_guard lock(mutex);const bool active=REXCVAR_GET(em2_discord_invites) && valid && !REXCVAR_GET(em2_private_profile);
        if(app!=id || enabled!=active){app=std::move(id);enabled=active;wake.notify_all();}
        if(active && !worker.joinable())worker=std::jthread([this](std::stop_token stop){Run(stop);});
    }
    void Set(std::string_view code,unsigned players,unsigned capacity,bool joinable){
        if(!Code(code) || capacity<2 || capacity>16 || !players || players>capacity)return;
        std::lock_guard lock(mutex);Party next{std::string(code),players,capacity,joinable};
        if(next!=wanted){wanted=std::move(next);++revision;invite_requested=false;invite_busy=false;wake.notify_all();}
    }
    void Clear(std::string_view code){std::lock_guard lock(mutex);if(wanted.code==code){wanted={};++revision;invite_requested=false;invite_busy=false;wake.notify_all();}}
    std::optional<std::string> Take(){std::lock_guard lock(mutex);if(pending.empty())return {};auto value=std::move(pending);pending.clear();return value;}
    std::string Status(){std::lock_guard lock(mutex);return status;}
    bool CanInvite(){std::lock_guard lock(mutex);return enabled && !invite_busy && confirmed_revision==revision && wanted.joinable && !wanted.code.empty() && wanted.players<wanted.capacity;}
    bool Invite(){
        std::lock_guard lock(mutex);
        if(!enabled || invite_busy || confirmed_revision!=revision || !wanted.joinable || wanted.code.empty() || wanted.players>=wanted.capacity)return false;
        invite_busy=true;invite_requested=true;wake.notify_all();return true;
    }
};
Discord& Service(){static Discord service;return service;}
}
void TickDiscordIntegration(){Service().Tick();}
void SetDiscordParty(std::string_view code,unsigned players,unsigned capacity,bool joinable){Service().Set(code,players,capacity,joinable);}
void ClearDiscordParty(std::string_view code){if(auto* service=live_service.load(std::memory_order_acquire))service->Clear(code);}
std::optional<std::string> TakeDiscordJoinCode(){return Service().Take();}
std::string DiscordConnectionStatus(){return Service().Status();}
bool CanInviteDiscordFriends(){return Service().CanInvite();}
bool InviteDiscordFriends(){return Service().Invite();}
}
