#include "online_room_probe.h"
#include "online_room_boot.h"
#include "online_native_menu.h"
#include "online_room_runtime.h"
#include "online_invite.h"
#include "online_local_movement.h"
#include "online_local_jump.h"
#include "online_actor_roster.h"
#include "online_room_gameplay.h"
#include "online_retail_input.h"
#include "simulation.h"
#include "native_character_physics.h"
#include "players.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/input/input_system.h>
#include <rex/input/input_driver.h>
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <bit>
#include <optional>

PPC_EXTERN_FUNC(__imp__sub_825430D8);
PPC_EXTERN_FUNC(__imp__sub_822F1C10);
PPC_EXTERN_FUNC(__imp__sub_82853C60);
PPC_EXTERN_FUNC(sub_82A27E20);
PPC_EXTERN_FUNC(sub_82A27F80);
REXCVAR_DEFINE_BOOL(em2_probe_native_policy_cycle,false,"Debug","Exact private four-player SDK fixture only: balanced native electric-stream restriction add at tick 300, remove at 500, second SDK tool attempt");
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_room_native_buttons);
REXCVAR_DECLARE(bool,em2_probe_online_chat);
REXCVAR_DECLARE(bool,em2_probe_room_recovery_controller);
REXCVAR_DECLARE(std::string,em2_user_data_directory);
REXCVAR_DECLARE(bool,em2_online_menu);
REXCVAR_DECLARE(bool,em2_online_qa_prelaunch);
REXCVAR_DECLARE(bool,em2_online_auto_continue);
REXCVAR_DECLARE(std::string,em2_online_qa_role);
REXCVAR_DECLARE(std::string,em2_native_lobby_qa_role);
REXCVAR_DECLARE(bool,em2_open_native_online_menu);
REXCVAR_DECLARE(bool,em2_probe_online_local_movement);
REXCVAR_DECLARE(bool,em2_probe_owned_native_player);
REXCVAR_DEFINE_STRING(em2_room_probe_role,"","Debug","Private native Room loopback probe: host or guest; never a gameplay adapter");
REXCVAR_DEFINE_STRING(em2_room_probe_invitation_file,"","Debug","Private native Room probe NEW absolute invitation path");
REXCVAR_DECLARE(uint32_t,em2_native_lobby_qa_damage_player);
REXCVAR_DECLARE(uint32_t,em2_probe_extra_damage_peer);
REXCVAR_DEFINE_INT32(em2_room_probe_expected_players,2,"Debug","Private native Room probe transport participant count 2..16; not playable actor capacity");
REXCVAR_DEFINE_BOOL(em2_room_probe_gameplay,false,"Debug","Explicit private two-retail-player native gameplay experiment; no public-menu enablement");
REXCVAR_DEFINE_BOOL(em2_room_probe_scripted_input,false,"Debug","Isolated verified loopback Room only: synthetic SDK source for extra-human movement and jump; never normal gameplay input");
REXCVAR_DEFINE_UINT32(em2_room_probe_input_warmup_ticks,0,"Debug","Private validated SDK input only: neutral native updates before the scripted program, maximum 600");
REXCVAR_DEFINE_BOOL(em2_room_probe_mixed_characters,false,"Debug","Private Room startup: odd extra peers choose Mickey and even extra peers choose Oswald through normal lobby requests");
REXCVAR_DEFINE_INT32(em2_room_probe_verification_window,1,"Debug","Private native Room completed-update credit 1..8; must match on all peers");
REXCVAR_DEFINE_STRING(em2_room_probe_replay_invitation_file,"","Debug","Host-only private loopback replay: reuse a captured session nonce while freshly verifying build/content/save");
namespace em2::net {
using rex::X_STATUS;
using rex::X_RESULT;
namespace {
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
std::atomic<bool> prepared{false};
Room* prepared_room=nullptr;
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
void Require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
bool Within(const fs::path& child,const fs::path& root){
    auto c=child.begin();for(auto r=root.begin();r!=root.end();++r,++c)
        if(c==child.end() || _wcsicmp(c->c_str(),r->c_str()))return false;
    return true;
}
void NoReparse(const fs::path& path){
    fs::path current;for(const auto& part:path){current/=part;
        const auto attributes=GetFileAttributesW(current.c_str());
        Require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Private Room probe rejects missing or reparse path ancestors");
    }
}
void PlainFile(HANDLE file){
    FILE_ATTRIBUTE_TAG_INFO info{};
    Require(GetFileInformationByHandleEx(file,FileAttributeTagInfo,&info,sizeof(info))!=0 &&
        !(info.FileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)),"Private Room invitation must be an ordinary file");
}
bool MissingFile(const fs::path& file){const auto attributes=GetFileAttributesW(file.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES)return false;
    const auto error=GetLastError();return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND;
}
void StoreNewInvitation(const fs::path& path,const std::string& code){
    const auto decoded=DecodeInvite(code);
    Require(decoded && decoded->address=="127.0.0.1" && code.size()<=256,"Private Room host produced an invalid loopback invitation");
    NoReparse(path.parent_path());
    Handle file{CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    Require(file.value!=INVALID_HANDLE_VALUE,"Private Room host could not CREATE_NEW invitation; an existing file is never replaced");
    PlainFile(file.value);DWORD written=0;
    Require(WriteFile(file.value,code.data(),DWORD(code.size()),&written,nullptr)!=0 && written==code.size() && FlushFileBuffers(file.value),
        "Private Room host could not finish its exact verified invitation");
}
std::optional<std::string> ReadInvitation(const fs::path& path){
    Handle file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    if(file.value==INVALID_HANDLE_VALUE){const auto error=GetLastError();
        if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND || error==ERROR_SHARING_VIOLATION)return {};
        throw std::runtime_error("Private Room guest cannot open invitation");
    }
    PlainFile(file.value);LARGE_INTEGER length{};
    Require(GetFileSizeEx(file.value,&length) && length.QuadPart>0 && length.QuadPart<=256,"Private Room invitation has invalid size");
    std::string code(size_t(length.QuadPart),'\0');DWORD got=0;
    Require(ReadFile(file.value,code.data(),DWORD(code.size()),&got,nullptr) && got==code.size(),"Private Room invitation read was incomplete");
    const auto decoded=DecodeInvite(code);
    Require(decoded && decoded->address=="127.0.0.1","Private Room guest requires a checksum-valid numeric loopback invitation");
    return code;
}
bool Readable(uint32_t pointer,size_t size,uint8_t* base){return base && pointer>=4096 && uint64_t(pointer)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+pointer,size);}
bool ObserveReadiness(PPCContext& ctx,uint8_t* base){
    Environment environment;
    struct Context {PPCContext& ctx;std::array<uint8_t,sizeof(PPCContext)> bytes{};
        explicit Context(PPCContext& value):ctx(value){std::memcpy(bytes.data(),&ctx,sizeof(ctx));}
        ~Context(){std::memcpy(&ctx,bytes.data(),sizeof(ctx));}} context(ctx);
    // Never assert readiness: the complete native actor/gameplay adapter is
    // deliberately absent. These are completed-tick observations, not controls.
    if(!prepared.load(std::memory_order_acquire) || GameplayInputGate().active() || !NativeRoomActive() || !em2::NativeCoopReady(base))return false;
    static thread_local uint32_t observed_registry=0;
    static thread_local Clock::time_point next{};
    const auto now=Clock::now();if(now<next)return false;next=now+std::chrono::seconds(1);
    if(!Readable(0x833DA7E4,4,base))return false;const auto registry=PPC_LOAD_U32(0x833DA7E4);
    if(!registry || registry==observed_registry)return false;
    std::array<NativeLocalMovementBinding,2> selected;
    for(unsigned slot=0;slot<2;++slot){
        struct Disarm {~Disarm(){DisarmNativeLocalJump();DisarmNativeLocalMovement();}} disarm;
        NativeLocalMovementBinding movement;bool found=false;
        for(unsigned device=0;device<4;++device){
            DisarmNativeLocalMovement();
            if(ArmNativeLocalMovement(1,registry,base,device,slot) && DiscoverNativeLocalMovementBinding(ctx,base,movement)){found=true;break;}
        }
        if(!found){const auto d=LocalMovementDiagnostics();
            REXLOG_INFO("Private Room probe: co-op admitted; P{} Movement not yet eligible, stage {} inspect {} event {:08X} count {} index {}",slot,d.discovery_stage,d.discovery_inspect_stage,d.discovery_event,d.discovery_count,d.discovery_index);return false;}
        NativeLocalJumpBinding jump;
        if(!DiscoverNativeLocalJumpBinding(movement,ctx,base,jump)){
            REXLOG_INFO("Private Room probe: P{} Movement lease eligible, native Jump pair not yet eligible; device {} action {:08X}",slot,movement.user,movement.action);return false;}
        std::array<uint8_t,800> controller{};std::memcpy(controller.data(),base+movement.controller,controller.size());
        std::array<uint8_t,sizeof(PPCContext)> incoming{};std::memcpy(incoming.data(),&ctx,sizeof(ctx));
        RoomMovement neutral;NativeLocalJumpEvents events;
        // Temporary private math-stream incarnation ONLY. No command, event,
        // spawn incarnation or readiness assertion is sent to the Room.
        const uint64_t diagnostic_epoch=0x50524F4245000000ull+slot+1;
        const bool math=ProduceNativeLocalRoomMovement(movement,{},diagnostic_epoch,ctx,base,neutral,movement.user) &&
            neutral.x==0 && neutral.y==0 && ArmNativeLocalJump(jump,diagnostic_epoch,0,ctx,base) &&
            ProduceNativeLocalJump(jump,{},diagnostic_epoch,0,ctx,base,events,movement.user) && events.count==0 && events.button_mask==jump.button_mask;
        if(!math || std::memcmp(controller.data(),base+movement.controller,controller.size()) || std::memcmp(incoming.data(),&ctx,sizeof(ctx))){
            REXLOG_ERROR("Private Room probe: P{} neutral ORIGINAL math/preservation failed; no command dispatched or submitted",slot);return false;}
        const auto diagnostics=LocalMovementDiagnostics();
        if(diagnostics.observations || diagnostics.captured){REXLOG_ERROR("Private Room probe: unexpected natural-capture provenance during P{} read-only discovery",slot);return false;}
        selected[slot]=movement;
        REXLOG_INFO("Private Room probe P{} eligible: entity {:08X}, controller {:08X}, listener {:08X}, Movement event/action {:08X}/{:08X}, logical user {}, native device {}, profile/user object {:08X}/{:08X}, reference {:08X}, forward {},{}; Jump/Release {:08X}/{:08X}, mask {:04X}; prior observations {} captures {}; neutral original math only",
            slot,movement.entity,movement.controller,movement.listener,movement.event,movement.action,movement.logical_user,movement.user,movement.profile,movement.user_object,movement.reference,neutral.forward_x,neutral.forward_z,
            jump.actions[0].action,jump.actions[1].action,jump.button_mask,diagnostics.observations,diagnostics.captured);
    }
    if(selected[0].entity==selected[1].entity)return false;
    observed_registry=registry;
    REXLOG_INFO("Private Room probe SUCCESS: real native co-op ready; BOTH retail slots independently discovered and neutral-evaluated with their own native references; P0/P1 handles {:08X}/{:08X}, native devices {}/{}; no source routing, events or network commands submitted; gameplay-ready vote remains FALSE",
        selected[0].handle,selected[1].handle,selected[0].user,selected[1].user);
    return false;
}
}
bool PreparePrivateRoomProbe(const fs::path& executable,const fs::path& content,fs::path& user_data){
    Environment environment;
    const auto role=REXCVAR_GET(em2_room_probe_role);if(role.empty())return true;
    OnlineRoomBoot boot;
    try{
        Require(!prepared.load() && (role=="host" || role=="guest"),"Private Room probe role must be host or guest and prepared once");
        const auto count=REXCVAR_GET(em2_room_probe_expected_players);
        Require(count>=2 && count<=16,"Private Room probe expects 2..16 transport participants");
        Require(REXCVAR_GET(em2_private_profile) && !REXCVAR_GET(em2_user_data_directory).empty(),"Private Room probe requires private_profile and explicit isolated user root");
        const auto window=REXCVAR_GET(em2_room_probe_verification_window);
        Require(window>=1 && window<=8 && boot.SetGameplayVerificationWindow(uint8_t(window)),"Private Room verification window must be 1..8");
        Require(REXCVAR_GET(em2_online_auto_continue),"Private Room probe requires native online automatic Continue enabled");
        Require(!REXCVAR_GET(em2_online_menu) && !REXCVAR_GET(em2_online_qa_prelaunch) && REXCVAR_GET(em2_online_qa_role).empty() &&
            REXCVAR_GET(em2_native_lobby_qa_role).empty() && !REXCVAR_GET(em2_open_native_online_menu) &&
            !REXCVAR_GET(em2_probe_online_local_movement) && !REXCVAR_GET(em2_probe_owned_native_player) &&
            !LegacyNativeSessionActive() && !NativeRoomActive(),"Private Room probe cannot overlap legacy QA, menus, native actor/input probes or session ownership");
        Require(executable.is_absolute() && content.is_absolute() && user_data.is_absolute(),"Private Room probe paths must be absolute");
        NoReparse(user_data);NoReparse(fs::path(REXCVAR_GET(em2_user_data_directory)));NoReparse(content);NoReparse(executable.parent_path());
        const auto program=fs::canonical(executable),assets=fs::canonical(content),user=fs::canonical(user_data);
        Require(_wcsicmp(program.filename().c_str(),L"beetleworx.exe")==0 && program.parent_path().filename()==L"runtime23" && program.parent_path().parent_path().filename()==L"build",
            "Private Room probe requires the canonical build/runtime23/beetleworx.exe");
        std::array<wchar_t,32768> running{};const auto length=GetModuleFileNameW(nullptr,running.data(),DWORD(running.size()));
        Require(length && length<running.size() && _wcsicmp(program.c_str(),fs::canonical(fs::path(running.data())).c_str())==0,
            "Private Room probe executable path must match this running executable");
        const auto analysis=fs::canonical(program.parent_path().parent_path().parent_path()/"analysis");
        Require(user.filename()==L"user_data" && user!=analysis && Within(user,analysis) && fs::canonical(REXCVAR_GET(em2_user_data_directory))==user,
            "Private Room probe source must be an explicit existing private analysis user_data copy");
        NoReparse(user);NoReparse(assets);NoReparse(program.parent_path());
        const auto target=user/"room-session";
        Require(MissingFile(target),"Private Room probe refuses an existing room-session destination");
        const auto configured=fs::path(REXCVAR_GET(em2_room_probe_invitation_file));
        Require(configured.is_absolute() && !configured.filename().empty(),"Private Room probe requires explicit absolute invitation file");
        const auto leaf=configured.filename().wstring();
        Require(leaf!=L"." && leaf!=L".." && leaf.size()<=128 && leaf.find(L':')==std::wstring::npos && leaf.find(L'\0')==std::wstring::npos,
            "Private Room invitation needs a plain bounded filename without alternate streams");
        NoReparse(configured.parent_path());const auto parent=fs::canonical(configured.parent_path());NoReparse(parent);
        Require(Within(parent,analysis),"Private Room invitation must stay inside private analysis roots");
        const auto invitation=parent/configured.filename();
        Require(invitation!=target && !Within(invitation,target) && !Within(invitation,user/"42560816"),"Private Room invitation cannot overwrite source/session campaign files");
        BootPaths paths{program,assets,target};paths.startup_policy=StartupPolicy::NativeMenu;
        // Share only the existing worker's verified content cache between the
        // private peers. FingerprintProgram still validates its complete key;
        // this never substitutes caller-supplied hashes or skips verification.
        NoReparse(analysis);
        const auto cache=analysis/"room-probe-content-identity.bin";
        Require(!Within(cache,assets) && !Within(cache,user),"Private Room identity cache must stay outside content and user data");
        const auto cache_attributes=GetFileAttributesW(cache.c_str());
        if(cache_attributes!=INVALID_FILE_ATTRIBUTES){
            Require(!(cache_attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)),
                "Private Room identity cache must be an ordinary file");
            NoReparse(cache);
        }else{
            const auto cache_error=GetLastError();
            Require(cache_error==ERROR_FILE_NOT_FOUND || cache_error==ERROR_PATH_NOT_FOUND,
                "Private Room identity cache path could not be validated");
        }
        paths.content_identity_cache=cache;
        if(role=="host"){
            Require(MissingFile(invitation),"Private Room host invitation already exists; never replacing it");
            const auto source=user/"42560816";NoReparse(source);Require(fs::is_directory(source),"Private Room host requires an already isolated campaign copy");
            std::optional<std::array<uint8_t,16>> replay;
            const auto replay_path=fs::path(REXCVAR_GET(em2_room_probe_replay_invitation_file));
            if(!replay_path.empty()){
                Require(replay_path.is_absolute(),"Private replay invitation path must be absolute");
                NoReparse(replay_path);const auto canonical=fs::canonical(replay_path);
                Require(Within(canonical,analysis) && canonical!=invitation,"Private replay requires an existing separate analysis invitation");
                const auto encoded=ReadInvitation(canonical);
                Require(bool(encoded),"Private replay invitation is not readable");
                const auto old=DecodeInvite(*encoded);
                Require(old && old->first_tick==0 && old->input_delay==2,"Private replay requires the same zero-start/two-tick timeline");
                replay=old->session;
                REXLOG_INFO("Private Room host replaying captured session nonce from {}; build/content/checkpoint are freshly measured, new port/invitation/root",canonical.string());
            }
            Require(boot.Host(paths,source,"127.0.0.1","127.0.0.1",uint8_t(count),"Room probe host",0,2,replay),"Private Room host preparation rejected");
        }else{
            Require(REXCVAR_GET(em2_room_probe_replay_invitation_file).empty(),"Only the private host selects the replay identity");
            const auto deadline=Clock::now()+std::chrono::seconds(180);std::optional<std::string> code;
            auto next_wait_log=Clock::now();
            while(Clock::now()<deadline){code=ReadInvitation(invitation);if(code)break;
                if(Clock::now()>=next_wait_log){REXLOG_INFO("Private Room guest waiting for NEW loopback invitation {}",invitation.string());next_wait_log=Clock::now()+std::chrono::seconds(5);}
                std::this_thread::sleep_for(std::chrono::milliseconds(50));}
            Require(bool(code),"Private Room guest invitation wait exceeded 180 seconds");
            Require(boot.Join(paths,*code,"Room probe guest "+std::to_string(GetCurrentProcessId())),"Private Room guest preparation rejected");
        }
        REXLOG_INFO("Private Room probe {} preparing verified build/content and NEW isolated shared save {}; loopback only, no gameplay adapter",role,target.string());
        const auto deadline=Clock::now()+std::chrono::seconds(180);auto next_log=Clock::now();bool published=false,ready=false,started=false;
        while(Clock::now()<deadline){
            boot.Poll();const auto status=boot.status();
            Require(status.state!=BootState::Failed && status.state!=BootState::Cancelled && status.state!=BootState::Cancelling,
                status.error.empty()?"Private Room preparation cancelled":status.error.c_str());
            if(role=="host" && !published && !status.invitation.empty()){StoreNewInvitation(invitation,status.invitation);published=true;
                REXLOG_INFO("Private Room host published the exact verified encoded invitation to NEW {}",invitation.string());}
            bool appearances_ready=true;
            if(REXCVAR_GET(em2_room_probe_mixed_characters)){
                const auto lobby=boot.lobby();const auto local=boot.local_peer();
                for(const auto& player:lobby.players){
                    const auto wanted=player.peer==1 || (player.peer>2 && (player.peer&1))?LobbyCharacter::Mickey:LobbyCharacter::Oswald;
                    if(player.character==wanted)continue;
                    appearances_ready=false;
                    if(player.peer==local && status.state==BootState::Ready && !boot.profile_pending())
                        Require(boot.SetAppearance(wanted,player.color),"Private Room native character choice rejected");
                }
            }
            // Appearance edits invalidate ready votes too. All peers wait for
            // the complete intended choices before voting through normal APIs.
            // A later roster Join invalidates every ready vote. Wait until the
            // exact requested roster is present before voting, including >2.
            if(!ready && appearances_ready && status.state==BootState::Ready && boot.room_state()==RoomState::Lobby &&
                boot.lobby().players.size()==size_t(count) && !boot.profile_pending()){
                Require(boot.SetReady(true),"Private Room local installed checkpoint could not Ready");ready=true;}
            if(role=="host" && !started && boot.lobby().players.size()==size_t(count) && boot.CanStart()){
                Require(boot.StartNativeBootstrap(),"Private Room host could not start native bootstrap");started=true;}
            if(auto result=boot.TakeStarted()){
                Require(result->room && result->room->native_bootstrap() && result->room->lobby().players.size()==size_t(count),"Private Room frozen roster/mode does not match requested diagnostic");
                auto* adopted=result->room.get();
                Require(AdoptNativeRoom(result->room,0),"Private Room native runtime refused frozen session ownership");
                prepared_room=adopted;
                if(!StartNativeRoomPrelaunchPolling()){ReleaseNativeRoomSession();throw std::runtime_error("Private Room could not start retained prelaunch polling");}
                user_data=result->user_data;prepared.store(true,std::memory_order_release);
                REXLOG_INFO("Private Room probe {} adopted real frozen native bootstrap; private Runtime user root {}",role,user_data.string());return true;
            }
            if(Clock::now()>=next_log){const auto progress=boot.checkpoint_progress();size_t installed=0;for(const auto& peer:progress)installed+=peer.installed;
                REXLOG_INFO("Private Room probe {} waiting: phase {} preparation {} peers {}/{} checkpoint-installed {}",role,int(status.state),int(status.preparation),boot.lobby().players.size(),count,installed);next_log=Clock::now()+std::chrono::seconds(5);}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("Private Room verified startup exceeded 180 seconds");
    }catch(const std::exception& error){boot.Cancel();REXLOG_ERROR("Private Room probe preparation failed: {}",error.what());return false;}
}
bool InstallPrivateRoomProbe(){
    Environment environment;if(REXCVAR_GET(em2_room_probe_role).empty())return true;
    if(REXCVAR_GET(em2_room_probe_gameplay)){
        if(!prepared.load(std::memory_order_acquire) || !EnableNativeRoomGameplay(0)){
            REXLOG_ERROR("Private native gameplay adapter configuration failed");return false;
        }
        REXLOG_INFO("Private native gameplay adapter configured: actual retail sources/receivers and completed-world proofs required before activation");
        return true;
    }
    if(!prepared.load(std::memory_order_acquire) || !NativeRoomActive() || !SetNativeRoomReadinessProbe(ObserveReadiness)){
        REXLOG_ERROR("Private Room probe could not install completed-tick observer on its bound native owner");return false;}
    REXLOG_INFO("Private Room probe installed: completed-tick observation only; gameplay readiness is NEVER asserted");return true;
}
std::unique_ptr<rex::input::InputSystem> MakePrivateRoomInput(Room& room){
    if(!REXCVAR_GET(em2_room_probe_scripted_input))return {};
    const bool probe=prepared.load(std::memory_order_acquire) && prepared_room==&room && REXCVAR_GET(em2_room_probe_gameplay);
    const bool menu=PrivateNativeMenuRoom()==&room;
    Require((probe || menu) && NativeRoomActive() &&
        REXCVAR_GET(em2_private_profile) && !REXCVAR_GET(em2_user_data_directory).empty() &&
        room.native_bootstrap() && room.lobby().locked && room.lobby().players.size()>=2,
        "Scripted Room input requires its exact prepared isolated loopback owner");
    const auto warmup=REXCVAR_GET(em2_room_probe_input_warmup_ticks);
    Require(warmup<=600,"Private SDK input warmup exceeds its bound");
    class Driver final:public rex::input::InputDriver {
        Room& room_;std::optional<uint64_t> first_,last_;unsigned reads_=0,warmup_=0,recovery_reads_=0;bool warming_=false;
    public:
        explicit Driver(Room& room,unsigned warmup):InputDriver(nullptr,0),room_(room),warmup_(warmup){}
        rex::X_STATUS Setup()override{return X_STATUS_SUCCESS;}
        rex::X_RESULT GetCapabilities(uint32_t,uint32_t,rex::input::X_INPUT_CAPABILITIES*)override{return X_ERROR_DEVICE_NOT_CONNECTED;}
        rex::X_RESULT GetState(uint32_t user,rex::input::X_INPUT_STATE* state)override{
            if(user || !state)return X_ERROR_DEVICE_NOT_CONNECTED;
            if(room_.state()!=RoomState::Running){
                if(!REXCVAR_GET(em2_probe_room_recovery_controller) || !REXCVAR_GET(em2_private_profile) ||
                   !NativeOnlineMenuQaActive() || PrivateNativeMenuRoom()!=&room_ ||
                   (room_.state()!=RoomState::Error && room_.state()!=RoomState::Disconnected))return X_ERROR_DEVICE_NOT_CONNECTED;
                *state={};const auto poll=recovery_reads_++;
                state->gamepad.buttons=(poll<2 || poll>=3)?0x1000:0;
                if(poll<=3)REXLOG_INFO("Private recovery SDK controller: peer {} poll {} buttons {}; held A, release, new A press",room_.local_peer(),poll,uint16_t(state->gamepad.buttons));
                return X_ERROR_SUCCESS;
            }
            *state={};++reads_;const auto tick=room_.next_tick();
            if(tick>=room_.identity().first_tick && tick-room_.identity().first_tick<warmup_){
                if(!warming_){warming_=true;REXLOG_INFO("Private Room SDK neutral warmup: peer {} first tick {} duration {}",room_.local_peer(),tick,warmup_);}
                return X_ERROR_SUCCESS;
            }
            if(!first_){first_=tick;if(warming_)REXLOG_INFO("Private Room SDK neutral warmup completed: peer {} program origin {}",room_.local_peer(),tick);}
            const auto elapsed=tick-*first_;const char* phase="neutral";
            const auto damage_player=REXCVAR_GET(em2_native_lobby_qa_damage_player);
            const auto extra_damage=REXCVAR_GET(em2_probe_extra_damage_peer);
            const bool damage_fixture=damage_player || extra_damage;
            const bool damage_target=room_.local_peer()==(damage_player?damage_player:extra_damage);
            const bool active=room_.lobby().players.size()>2?room_.local_peer()>=3:room_.local_peer()==2;
            const int direction=room_.lobby().players.size()>2 && !(room_.local_peer()&1)?-1:1;
            if(active){
                if(elapsed>=60 && elapsed<72){state->gamepad.thumb_ly=int16_t(direction*16000);phase=direction>0?"forward":"backward";}
                if(elapsed>=90 && elapsed<102){state->gamepad.thumb_ly=int16_t(-direction*16000);phase=direction>0?"backward":"forward";}
                if(elapsed>=130 && elapsed<134){state->gamepad.buttons=0x1000;phase="jump";}
                if(elapsed>=175 && elapsed<195){state->gamepad.thumb_rx=int16_t(direction*20000);phase=direction>0?"look right":"look left";}
                if(elapsed>=240 && elapsed<252){state->gamepad.thumb_ly=int16_t(direction*16000);phase=direction>0?"forward after look":"backward after look";}
                if(elapsed>=300 && elapsed<320){state->gamepad.thumb_rx=int16_t(-direction*20000);phase=direction>0?"look left":"look right";}
                if(REXCVAR_GET(em2_probe_online_chat) && elapsed>=480 && elapsed<492){state->gamepad.thumb_ly=int16_t(direction*16000);phase="move after chat";}
            }
            if(REXCVAR_GET(em2_room_native_buttons) && active){
                if(elapsed>=280 && elapsed<284){state->gamepad.buttons=0x4000;phase="tool button X";}
                if(elapsed>=330 && elapsed<345){state->gamepad.right_trigger=200;phase="tool trigger RT";}
                if(REXCVAR_GET(em2_probe_native_policy_cycle) && !(room_.local_peer()&1) && elapsed>=530 && elapsed<545){state->gamepad.right_trigger=200;phase="tool after policy release";}
            }
            if(damage_target){
                if(extra_damage && elapsed>=720 && elapsed<760){state->gamepad.thumb_ly=16000;phase="movement while dead";}
                if(extra_damage && elapsed>=740 && elapsed<744){state->gamepad.buttons=0x1000;phase="jump while dead";}
                if(elapsed>=1000 && elapsed<1012){state->gamepad.thumb_ly=16000;phase="forward after respawn";}
                if(elapsed>=1040 && elapsed<1052){state->gamepad.thumb_ly=-16000;phase="backward after respawn";}
                if(elapsed>=1080 && elapsed<1084){state->gamepad.buttons=0x1000;phase="jump after respawn";}
            }
            const auto complete=damage_fixture?1200u:REXCVAR_GET(em2_probe_native_policy_cycle)?560u:360u;
            if((!last_ || *last_!=tick) && ((extra_damage && (elapsed==720 || elapsed==740 || elapsed==744 || elapsed==760)) || (damage_fixture && (elapsed==1000 || elapsed==1012 || elapsed==1040 || elapsed==1052 || elapsed==1080 || elapsed==1084 || elapsed==1200)) || elapsed==0 || elapsed==60 || elapsed==72 || elapsed==90 || elapsed==102 || elapsed==130 || elapsed==134 || elapsed==175 || elapsed==195 || elapsed==240 || elapsed==252 || elapsed==280 || elapsed==284 || elapsed==300 || elapsed==320 || elapsed==330 || elapsed==345 || elapsed==360 || (REXCVAR_GET(em2_probe_native_policy_cycle) && (elapsed==530 || elapsed==545 || elapsed==560)) || (REXCVAR_GET(em2_probe_online_chat) && (elapsed==480 || elapsed==492))))
                REXLOG_INFO("Private Room SDK input: peer {} tick {} elapsed {} phase '{}' reads {}; synthetic SDK driver, original native interpreters",room_.local_peer(),tick,elapsed,elapsed==complete?"complete":phase,reads_);
            last_=tick;return X_ERROR_SUCCESS;
        }
        rex::X_RESULT SetState(uint32_t,rex::input::X_INPUT_VIBRATION*)override{return X_ERROR_DEVICE_NOT_CONNECTED;}
        rex::X_RESULT GetKeystroke(uint32_t,uint32_t,rex::input::X_INPUT_KEYSTROKE*)override{return X_ERROR_EMPTY;}
    };
    auto input=std::make_unique<rex::input::InputSystem>(nullptr);input->AddDriver(std::make_unique<Driver>(room,warmup));
    REXLOG_INFO("Private Room synthetic SDK source installed: peer {}; real hardware and normal session untouched",room.local_peer());return input;
}
void TickPrivateNativeTaskPolicy(PPCContext& ctx,uint8_t* base){
    if(!REXCVAR_GET(em2_probe_native_policy_cycle))return;
    Environment environment;
    if(!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_room_probe_scripted_input) ||
       !REXCVAR_GET(em2_room_native_buttons) || !NativeOnlineMenuQaActive() || ctx.lr!=0x829ED694)return;
    uint64_t tick=0;if(!em2::CurrentAgreedSimulationTick(tick) || (tick!=300 && tick!=500))return;
    auto* room=PrivateNativeMenuRoom();auto* game=CurrentNativeRoomGameplay();
    if(!room || !game || room->state()!=RoomState::Running || room->lobby().players.size()!=4 ||
       !room->lobby().locked || game->extra_actor_mask()!=12 || !em2::NativeCoopReady(base))return;
    static thread_local Room* owner=nullptr;static thread_local unsigned stage=0;
    if(owner!=room){owner=room;stage=0;}
    auto reject=[&](const char* why){REXLOG_ERROR("Private native task policy cycle rejected: {}",why);game->ObservationFailed();};
    if(stage!=(tick==300?0u:1u)){reject("unexpected balanced-cycle stage");return;}
    RetailActorInput retail;
    if(!RetailActorInputFor(1,ctx,base,retail)){reject("original Oswald input is not owned");return;}
    struct Borrow {
        PPCContext& ctx;PPCContext saved;uint8_t* memory=nullptr;uint32_t scratch=0;
        std::array<uint8_t,32768> bytes{};
        Borrow(PPCContext& c,uint8_t* base):ctx(c),saved(c){
            const auto top=c.r1.u32&~15u;
            if(top>=65536 && Readable(top-32768,bytes.size(),base)){
                memory=base+top-32768;std::memcpy(bytes.data(),memory,bytes.size());
                ctx.r1.u64=top-4096;PPC_STORE_U32(ctx.r1.u32,saved.r1.u32);scratch=top-1024;
            }
        }
        ~Borrow(){if(memory)std::memcpy(memory,bytes.data(),bytes.size());ctx=saved;}
    }borrow(ctx,base);
    if(!borrow.memory){reject("borrowed native stack unavailable");return;}
    const auto name=borrow.scratch,reason=name+4,text=name+128;
    constexpr char task_name[]="ElectricStream",why[]="Private balanced policy cycle";
    std::memcpy(base+text,task_name,sizeof(task_name));
    ctx.r3.u64=text;__imp__sub_825430D8(ctx,base);PPC_STORE_U32(name,ctx.r3.u32);
    std::memcpy(base+text,why,sizeof(why));
    ctx.r3.u64=text;__imp__sub_825430D8(ctx,base);PPC_STORE_U32(reason,ctx.r3.u32);
    auto release=[&]{ctx.r3.u64=reason;__imp__sub_822F1C10(ctx,base);ctx.r3.u64=name;__imp__sub_822F1C10(ctx,base);};
    ctx.r3.u64=retail.controller+156;ctx.r4.u64=name;__imp__sub_82853C60(ctx,base);const auto task=ctx.r3.u32;
    if(!Readable(task,224,base) || PPC_LOAD_U32(task)!=0x82164EA8){release();reject("original electric-stream task is unavailable");return;}
    const auto before=PPC_LOAD_U32(task+8),expected=tick==300?0u:2u;
    if((before&14)!=expected){release();reject("restriction count differs from the one balanced test owns");return;}
    ctx.r3.u64=retail.manager;ctx.r4.u64=1;ctx.r5.u64=name;ctx.r6.u64=reason;
    if(tick==300)sub_82A27E20(ctx,base);else sub_82A27F80(ctx,base);
    const auto after=PPC_LOAD_U32(task+8);release();
    if((after&14)!=(tick==300?2u:0u)){reject("original native restriction writer did not change exactly one count");return;}
    ++stage;
    REXLOG_INFO("Private native task policy cycle: tick {} operation {} profile 1 task ElectricStream before {:08X} after {:08X}; original writer, balanced test only",tick,tick==300?"add":"remove",before,after);
}
void ObservePrivateRoomActors(uint64_t tick,std::span<const OwnedRosterActor> actors,uint8_t* base){
    if(!REXCVAR_GET(em2_room_probe_scripted_input) || !REXCVAR_GET(em2_private_profile) || !NativeRoomActive())return;
    auto* room=prepared.load(std::memory_order_acquire)?prepared_room:PrivateNativeMenuRoom();
    if(!room || room->state()!=RoomState::Running)return;
    Environment environment;
    for(const auto& entry:actors){GameplayObservation::CharacterPhysics sample;
        if(!em2::ReadNativeActorPhysics(base,entry.actor.entity,sample)){
            REXLOG_ERROR("Private Room actor trajectory unavailable: room peer {} tick {}",entry.room_peer,tick);continue;}
        REXLOG_INFO("Private Room actor trajectory: room peer {} tick {} position {}/{}/{} velocity {}/{}/{} native state {} active {} consumed {}",entry.room_peer,tick,
            std::bit_cast<float>(sample.position[0]),std::bit_cast<float>(sample.position[1]),std::bit_cast<float>(sample.position[2]),
            std::bit_cast<float>(sample.velocity[0]),std::bit_cast<float>(sample.velocity[1]),std::bit_cast<float>(sample.velocity[2]),sample.state,sample.active,
            OwnedActorBasisConsumed(entry.actor.peer,entry.actor.generation,tick));
        const auto entity=entry.reference.reference;
        if(Readable(entity,96,base) && PPC_LOAD_U32(entity)==0x8208D908 && PPC_LOAD_U32(entity+44)==2){
            const auto list=PPC_LOAD_U32(entity+36);
            const auto component=Readable(list,8,base)?PPC_LOAD_U32(list+4):0;
            const auto node=Readable(component,84,base) && PPC_LOAD_U32(component)==0x820D8970?PPC_LOAD_U32(component+80):0;
            if(Readable(node,368,base) && PPC_LOAD_U32(node)==0x8203E610){
                REXLOG_INFO("Private Room owned camera pose: room peer {} tick {} node {:08X} position {}/{}/{}",entry.room_peer,tick,node,
                    std::bit_cast<float>(PPC_LOAD_U32(node+192)),std::bit_cast<float>(PPC_LOAD_U32(node+196)),std::bit_cast<float>(PPC_LOAD_U32(node+200)));
                REXLOG_INFO("Private Room owned camera basis: room peer {} tick {} node {:08X} forward {}/{}/{}",entry.room_peer,tick,node,
                    std::bit_cast<float>(PPC_LOAD_U32(node+176)),std::bit_cast<float>(PPC_LOAD_U32(node+180)),std::bit_cast<float>(PPC_LOAD_U32(node+184)));
            }
        }
    }
    // Native scene-camera manager owns the two retail output NiEntities.
    // Read their actual nodes to detect extra Look2 spilling into retail P2.
    const auto scene=Readable(0x833DF47C,4,base)?PPC_LOAD_U32(0x833DF47C):0;
    if(Readable(scene,12,base))for(unsigned slot=0;slot<2;++slot){
        const auto root=PPC_LOAD_U32(scene+(slot+1)*4),entity=root?root+8:0;
        if(!Readable(entity,96,base) || PPC_LOAD_U32(entity)!=0x8208D908 || PPC_LOAD_U32(entity+44)!=2)continue;
        const auto list=PPC_LOAD_U32(entity+36),component=Readable(list,8,base)?PPC_LOAD_U32(list+4):0;
        const auto node=Readable(component,84,base) && PPC_LOAD_U32(component)==0x820D8970?PPC_LOAD_U32(component+80):0;
        if(Readable(node,368,base) && PPC_LOAD_U32(node)==0x8203E610)
            REXLOG_INFO("Private Room retail camera basis: slot {} tick {} node {:08X} forward {}/{}/{}",slot,tick,node,
                std::bit_cast<float>(PPC_LOAD_U32(node+176)),std::bit_cast<float>(PPC_LOAD_U32(node+180)),std::bit_cast<float>(PPC_LOAD_U32(node+184)));
    }
}
}
