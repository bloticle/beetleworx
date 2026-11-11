#pragma once
#include "online_boot.h"
#include "online_room.h"
namespace em2::net {
struct RoomBootResult {
    std::unique_ptr<Room> room;
    std::filesystem::path user_data;
};
// Single owner, asynchronous shared-save preparation for a larger Room.
// It neither loads a native world nor asserts WorldReady. Ready means the
// shared save is installed and lobby editing is allowed. TakeStarted returns
// only a frozen Loading room, exactly once, for a future native-world owner.
class OnlineRoomBoot {
public:
    OnlineRoomBoot();
    ~OnlineRoomBoot();
    OnlineRoomBoot(const OnlineRoomBoot&)=delete;
    OnlineRoomBoot& operator=(const OnlineRoomBoot&)=delete;
    bool Host(BootPaths,const std::filesystem::path& frozen_checkpoint,
        std::string listen_address,std::string advertised_address,uint8_t gameplay_capacity,
        std::string name="Player 1",uint16_t port=0,uint8_t input_delay=2);
    bool Join(BootPaths,std::string invitation,std::string name);
    void Poll();
    void Cancel();
    BootStatus status()const;
    RoomState room_state()const;
    LobbySnapshot lobby()const;
    std::vector<RoomCheckpointPeer> checkpoint_progress()const;
    LobbyResult last_lobby_result()const;
    bool profile_pending()const;
    bool Rename(std::string_view);
    bool SetAppearance(LobbyCharacter,uint8_t color);
    bool SetReady(bool);
    bool CanStart()const;
    bool Start();
    std::optional<RoomBootResult> TakeStarted();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool TestOnlineRoomBoot(const std::filesystem::path& fixture);
}
