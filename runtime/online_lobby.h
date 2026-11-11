#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace em2::net {
enum class LobbyCharacter : uint8_t { Mickey, Oswald };
struct LobbyColor { std::string_view name; uint32_t rgba; };
// The identical RGBA value drives both the pants tint and the name label.
inline constexpr std::array<LobbyColor,16> kLobbyColors{{
    {"Red",0xEE4545FF}, {"Blue",0x4285F4FF}, {"Green",0x43BC68FF},
    {"Yellow",0xF4D447FF}, {"Purple",0xB678E8FF}, {"Orange",0xF39A3EFF},
    {"Cyan",0x42D5DCFF}, {"Pink",0xF181B5FF},
    {"Crimson",0xA91E3AFF}, {"Navy",0x233B80FF}, {"Mint",0x9CE7C1FF},
    {"Lime",0xB4D63FFF}, {"Lavender",0xCDBCF6FF}, {"Brown",0x96683FFF},
    {"White",0xF3F1E8FF}, {"Slate",0x778695FF}
}};
static_assert([]{
    for(std::size_t i=0;i<kLobbyColors.size();++i)
        for(std::size_t j=0;j<i;++j)if(kLobbyColors[i].rgba==kLobbyColors[j].rgba)return false;
    return true;
}(),"Every lobby color must have a distinct pants/name-label RGBA value");
struct LobbyPlayer {
    uint64_t peer=0;
    std::string name;
    LobbyCharacter character=LobbyCharacter::Oswald;
    uint8_t color=0;
    bool ready=false;
    bool operator==(const LobbyPlayer&) const = default;
};
enum class LobbyResult { Ok, Full, DuplicatePeer, InvalidName, UnknownPeer,
    ColorInUse, InvalidCharacter, InvalidColor, Locked, NotReady };
// Host-owned roster. Capacity is supplied by the actual gameplay adapter;
// supporting sixteen roster entries does not assert sixteen native player actors.
class LobbyRoster {
public:
    explicit LobbyRoster(uint8_t gameplay_capacity);
    LobbyResult Join(uint64_t peer,std::string_view name);
    LobbyResult Leave(uint64_t peer);
    LobbyResult Rename(uint64_t peer,std::string_view name);
    LobbyResult SetAppearance(uint64_t peer,LobbyCharacter character,uint8_t color);
    LobbyResult SetReady(uint64_t peer,bool ready);
    LobbyResult Start();
    void Unlock();
    const LobbyPlayer* Find(uint64_t peer) const;
    const std::vector<LobbyPlayer>& players() const { return players_; }
    uint8_t capacity() const { return capacity_; }
    uint64_t revision() const { return revision_; }
    bool locked() const { return locked_; }
private:
    void Changed(bool invalidate_readiness);
    uint8_t capacity_;
    uint64_t revision_=0;
    bool locked_=false;
    std::vector<LobbyPlayer> players_;
};
struct LobbySnapshot {
    uint64_t revision=0;
    uint8_t capacity=0;
    bool locked=false;
    std::vector<LobbyPlayer> players;
    bool operator==(const LobbySnapshot&) const = default;
};
bool TestOnlineLobby();
}
