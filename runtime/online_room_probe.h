#pragma once
#include <filesystem>
#include <memory>
#include <span>
#include <cstdint>
namespace rex::input {class InputSystem;}
namespace em2::net {
class Room;
struct OwnedRosterActor;
// Private loopback diagnostics only. No role means a successful no-op.
// Call before Runtime construction; user_data changes only after real verified
// checkpoint installation and frozen Room adoption succeed.
bool PreparePrivateRoomProbe(const std::filesystem::path& executable,
    const std::filesystem::path& content,std::filesystem::path& user_data);
// Call after BindNativeRoomThread. Default observation mode always votes false.
// Explicit private gameplay mode instead installs the native Room adapter.
bool InstallPrivateRoomProbe();
// Explicit isolated loopback only. Supplies a counted synthetic SDK driver;
// ordinary launches keep the real runtime input system. No wire pointers or
// post-interpreter values are injected into authoritative frames.
std::unique_ptr<rex::input::InputSystem> MakePrivateRoomInput(Room&);
void ObservePrivateRoomActors(uint64_t tick,std::span<const OwnedRosterActor>,uint8_t*);
}
