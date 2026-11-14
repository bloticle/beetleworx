#pragma once
#include "online.h"
#include <filesystem>
namespace em2::net {
enum class BootState {Idle,Preparing,WaitingForPeer,Connecting,Transferring,Installing,Ready,Cancelling,Cancelled,Failed,Taken};
enum class PreparationStep {CheckingFiles,PackingSave,InstallingHostSave};
struct BootPaths {
    std::filesystem::path executable,content,new_user_data;
    StartupPolicy startup_policy=StartupPolicy::Prelaunch;
    // Nonempty only for native Resume: revalidate the retained host archive on
    // the preparation worker before fingerprinting or creating a destination.
    std::filesystem::path retained_host_normal_root;
    std::filesystem::path content_identity_cache;
};
struct BootStatus {
    BootState state=BootState::Idle;
    PreparationStep preparation=PreparationStep::CheckingFiles;
    std::string invitation,error;
    std::filesystem::path user_data;
};
struct BootResult {
    std::unique_ptr<Session> session;
    std::filesystem::path user_data;
};
// Pre-game, single-owner coordinator. Poll while displaying the connection UI;
// hashing/save I/O runs on workers. The guest must not run against these paths
// before Ready. Keep polling during runtime preparation, then transfer ownership
// once to the simulation owner. No destructor stops or modifies a guest runtime.
class OnlineBoot {
public:
    OnlineBoot();~OnlineBoot();
    OnlineBoot(const OnlineBoot&)=delete;OnlineBoot& operator=(const OnlineBoot&)=delete;
    bool Host(BootPaths,const std::filesystem::path& frozen_checkpoint,
              std::string listen_address,std::string advertised_address,uint16_t port=0,uint8_t input_delay=2,
              std::optional<std::array<uint8_t,16>> session_nonce={});
    bool Join(BootPaths,std::string invitation);
    void Poll();
    void Cancel();
    BootStatus status() const;
    LobbySnapshot lobby() const;
    bool RenameLobbyPlayer(std::string_view);
    bool SetLobbyReady(bool);
    bool lobby_request_pending() const;
    LobbyResult last_lobby_result() const;
    bool ArmLobbyStart();
    bool CanRequestLobbyStart() const;
    bool RequestLobbyStart();
    bool LobbyStartConfirmed() const;
    std::optional<BootResult> TakeReady();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
bool TestOnlineBoot(const std::filesystem::path& fixture);
}
