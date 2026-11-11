#include "online.h"
#include "online_runtime.h"
#include "online_world_random.h"
#include "players.h"
#include "controls.h"
#include "online_invite.h"
#include "online_checkpoint.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/kernel/kernel_state.h>
#include <rex/input/input_system.h>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <immintrin.h>
#include <windows.h>
#include <future>
#include <thread>

REXCVAR_DECLARE(std::string,em2_user_data_directory);
REXCVAR_DECLARE(std::string,em2_identity_checkpoint_directory);
REXCVAR_DECLARE(std::string,em2_checkpoint_install_directory);
REXCVAR_DECLARE(std::string,em2_checkpoint_bundle_output);
REXCVAR_DEFINE_BOOL(em2_online_qa_prelaunch,false,"Debug","Connect localhost QA peers before guest startup and use retained native online ownership");
REXCVAR_DEFINE_STRING(em2_online_qa_role,"","Debug","Development-only localhost two-world test: host or client");
REXCVAR_DEFINE_STRING(em2_online_qa_identity_file,"","Debug","Shared preverified file identity/nonce for isolated localhost QA worlds");
REXCVAR_DEFINE_INT32(em2_online_qa_port,41036,"Debug","Localhost two-world QA port");
REXCVAR_DEFINE_STRING(em2_online_qa_invite_file,"","Debug","Optional localhost invitation file: host publishes, client reads with its independent local identity");

namespace em2::net {
namespace {
std::optional<Invite> LoadInvite(const std::filesystem::path& path) {
    std::error_code error;if(!path.is_absolute() || std::filesystem::file_size(path,error)>128 || error)return {};
    std::ifstream file(path,std::ios::binary);std::array<char,129> text{};file.read(text.data(),text.size());
    if(file.bad() || file.gcount()>128)return {};
    return DecodeInvite(std::string_view(text.data(),size_t(file.gcount())));
}
bool StoreInvite(const std::filesystem::path& path,const Invite& invite) {
    const auto code=EncodeInvite(invite);if(!path.is_absolute() || !code)return false;
    auto temporary=path;temporary+=".tmp";
    std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file<<*code<<'\n';file.close();
    return file && MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
bool LoadIdentity(const std::filesystem::path& path,Identity& identity) {
    std::error_code error;
    if(!path.is_absolute() || std::filesystem::file_size(path,error)>512 || error) return false;
    std::ifstream file(path);std::string header,line;
    if(!std::getline(file,header))return false;
    if(!header.empty() && header.back()=='\r')header.pop_back();
    if(header!="EM2-QA-IDENTITY-1")return false;
    auto read=[&](auto& bytes) {
        if(!std::getline(file,line))return false;
        if(!line.empty() && line.back()=='\r')line.pop_back();
        if(line.size()!=bytes.size()*2)return false;
        for(size_t i=0;i<bytes.size();++i) {
            unsigned value=0;const auto parsed=std::from_chars(line.data()+i*2,line.data()+i*2+2,value,16);
            if(parsed.ec!=std::errc{} || parsed.ptr!=line.data()+i*2+2)return false;
            bytes[i]=uint8_t(value);
        }
        return true;
    };
    if(!read(identity.build) || !read(identity.content) || !read(identity.checkpoint) || !read(identity.session))return false;
    while(std::getline(file,line))if(!line.empty() && line!="\r")return false;
    return true;
}
std::unique_ptr<Session> OpenLocalQASession() {
    const auto role=REXCVAR_GET(em2_online_qa_role);
    const auto port=REXCVAR_GET(em2_online_qa_port);
    Identity identity;
    if((role!="host" && role!="client") || REXCVAR_GET(em2_user_data_directory).empty() || port<1024 || port>65535 ||
       !LoadIdentity(REXCVAR_GET(em2_online_qa_identity_file),identity)) {
        REXLOG_ERROR("Localhost QA requires host/client role, isolated saves, valid port and shared preverified identity file");return {};
    }
    const auto invite_path=REXCVAR_GET(em2_online_qa_invite_file);
    identity.startup_policy=StartupPolicy::Prelaunch;
    identity.initialization_policy=ConfiguredNativeInitializationPolicy();
    std::string address="127.0.0.1";auto connect_port=uint16_t(port);
    if(!invite_path.empty()) {
        if(role=="host") {
            if(!NewSessionNonce(identity.session)){REXLOG_ERROR("Localhost QA could not generate session nonce");return {};}
        } else {
            const auto invite=LoadInvite(invite_path);
            const auto local=invite?IdentityForInvite(identity,*invite):std::nullopt;
            if(!local || invite->address!="127.0.0.1") {
                REXLOG_ERROR("Localhost QA requires a valid localhost invitation and independent local compatibility identity");return {};
            }
            identity=*local;address=invite->address;connect_port=invite->port;
            REXLOG_INFO("Localhost QA client accepted invitation endpoint/session; retained local build/content/checkpoint hashes");
        }
    }
    auto session=std::make_unique<Session>();
    const bool opened=role=="host"?session->Host(identity,address,connect_port):session->Join(identity,address,connect_port);
    if(!opened){REXLOG_ERROR("Localhost QA open failed: {}",session->error());return {};}
    if(role=="host" && !invite_path.empty()) {
        const Invite invite{address,session->port(),identity.session,identity.first_tick,identity.input_delay};
        if(!StoreInvite(invite_path,invite)){session->Disconnect();REXLOG_ERROR("Localhost QA could not publish invitation file");return {};}
        REXLOG_INFO("Localhost QA host published invitation with a fresh OS-generated nonce");
    }
    REXLOG_INFO("Localhost QA {} started on port {}; native world synchronization is under test",role,connect_port);
    return session;
}
}
bool PrepareNativeOnlineQASession() {
    if(!REXCVAR_GET(em2_online_qa_prelaunch))return true;
    auto session=OpenLocalQASession();if(!session)return false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(std::chrono::steady_clock::now()<deadline) {
        session->Poll();
        if(session->state()==State::Ready) {
            if(!AdoptNativeOnlineSession(session) || !StartNativeOnlinePrelaunchPolling())return false;
            REXLOG_INFO("Localhost QA retained session ready before runtime creation; native startup owns subsequent preparation");return true;
        }
        if(session->state()==State::Error || session->state()==State::Disconnected) {REXLOG_ERROR("Localhost QA prelaunch failed: {}",session->error());return false;}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REXLOG_ERROR("Localhost QA prelaunch timed out waiting for peer");return false;
}
bool RunCheckpointTransferDiagnostic() {
    const auto role=REXCVAR_GET(em2_online_qa_role);const bool host=role=="host";
    const auto port=REXCVAR_GET(em2_online_qa_port);Identity identity;
    const std::filesystem::path invitation_path=REXCVAR_GET(em2_online_qa_invite_file),install_path=REXCVAR_GET(em2_checkpoint_install_directory);
    if((!host && role!="client") || port<1024 || port>65535 || !invitation_path.is_absolute() ||
       !LoadIdentity(REXCVAR_GET(em2_online_qa_identity_file),identity) || (!host && !install_path.is_absolute())) {
        REXLOG_ERROR("Checkpoint preflight requires host/client role, invitation path, local identity and client new install directory");return false;
    }
    Session session;
    if(host) {
        const std::filesystem::path source=REXCVAR_GET(em2_identity_checkpoint_directory);
        if(!source.is_absolute()){REXLOG_ERROR("Checkpoint preflight requires a frozen absolute source");return false;}
        auto packed=PackCheckpoint(source);
        if(!packed.value || packed.value->identity!=identity.checkpoint || !NewSessionNonce(identity.session)) {
            REXLOG_ERROR("Checkpoint preflight pack/identity failed: {}",packed.error);return false;
        }
        auto bundle=std::make_shared<CheckpointBundle>(std::move(*packed.value));
        if(!session.HostWithCheckpoint(identity,"127.0.0.1",uint16_t(port),bundle) ||
           !StoreInvite(invitation_path,Invite{"127.0.0.1",session.port(),identity.session,identity.first_tick,identity.input_delay})) {
            REXLOG_ERROR("Checkpoint preflight host open/invitation failed: {}",session.error());return false;
        }
        REXLOG_INFO("Checkpoint preflight host listening with {} bundle bytes",bundle->bytes.size());
    } else {
        const auto invite=LoadInvite(invitation_path);const auto local=invite?IdentityForInvite(identity,*invite,true):std::nullopt;
        if(!local || invite->address!="127.0.0.1" || !session.JoinWithCheckpoint(*local,invite->address,invite->port)) {
            REXLOG_ERROR("Checkpoint preflight client invitation/connect failed: {}",session.error());return false;
        }
    }
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    std::future<CheckpointInstallResult> installer;std::stop_source cancellation;bool installing=false;
    std::optional<std::chrono::steady_clock::time_point> ready;
    while(std::chrono::steady_clock::now()<deadline) {
        session.Poll();
        if(!host && !installing)if(auto bytes=session.DownloadedCheckpoint()) {
            const std::filesystem::path output=REXCVAR_GET(em2_checkpoint_bundle_output);
            if(!output.empty() && !WriteNewCheckpointBundle(*bytes,output)) {
                session.ConfirmCheckpointInstalled(false);REXLOG_ERROR("Checkpoint preflight cannot persist received bundle");return false;
            }
            const auto expected=session.identity().checkpoint;const auto stop=cancellation.get_token();
            installer=std::async(std::launch::async,[bytes,expected,install_path,stop]{return InstallCheckpoint(*bytes,expected,install_path,stop);});installing=true;
        }
        if(installing && installer.valid() && installer.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            const auto result=installer.get();
            if(!result.installed || !session.ConfirmCheckpointInstalled(true)) {
                session.ConfirmCheckpointInstalled(false);REXLOG_ERROR("Checkpoint preflight installation failed: {}",result.error);return false;
            }
            REXLOG_INFO("Checkpoint preflight client installed network bundle into {}",install_path.string());
        }
        const auto now=std::chrono::steady_clock::now();
        if(session.state()==State::Ready && !ready){ready=now;REXLOG_INFO("Checkpoint preflight {} ready after verified installation acknowledgement",role);}
        if(session.state()==State::Error || session.state()==State::Disconnected) {
            if(ready && session.state()==State::Disconnected)return true;
            cancellation.request_stop();REXLOG_ERROR("Checkpoint preflight {} failed: {}",role,session.error());return false;
        }
        if(ready && now-*ready>=std::chrono::milliseconds(100))return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cancellation.request_stop();REXLOG_ERROR("Checkpoint preflight timed out");return false;
}
void UpdateOnlineQASession(uint8_t* base) {
    if(REXCVAR_GET(em2_online_qa_prelaunch))return;
    const auto role=REXCVAR_GET(em2_online_qa_role);if(role.empty())return;
    struct Restore {unsigned csr=_mm_getcsr();~Restore(){_mm_setcsr(csr);}} restore;
    static std::unique_ptr<Session> session;
    static bool initialized=false,terminal=false,attached=false,ready_logged=false,preparing=false;
    if(terminal)return;
    if(!initialized) {initialized=true;session=OpenLocalQASession();if(!session){terminal=true;return;}}
    session->Poll();
    if(session->state()==State::Error || session->state()==State::Disconnected) {
        em2::SetOnlineJoinControls(false);
        REXLOG_ERROR("Localhost QA {} session ended: {}",role,session->error());terminal=true;return;
    }
    if(session->state()!=State::Ready)return;
    if(!ready_logged){REXLOG_INFO("Localhost QA {} transport ready; waiting for native two-human gameplay",role);ready_logged=true;}
    if(!attached && !em2::NativeCoopReady(base)) {
        const bool allowed=em2::NativeOnlineJoinAllowed(base);
        if(allowed && !preparing){preparing=true;REXLOG_INFO("Localhost QA {} preparing native co-op automatically",role);}
        if(preparing)em2::SetOnlineJoinControls(true,allowed);
        return;
    }
    if(!attached && em2::NativeCoopReady(base)) {
        em2::SetOnlineJoinControls(false);
        auto* kernel=rex::kernel::kernel_state();auto* input=kernel?kernel->input_system():nullptr;
        if(!input || !GameplayInputGate().Attach(*session,*input,0)) {
            REXLOG_ERROR("Localhost QA {} could not attach native world",role);terminal=true;return;
        }
        attached=true;REXLOG_INFO("Localhost QA {} native world attached at agreed tick {}",role,session->identity().first_tick);
    }
}
}
