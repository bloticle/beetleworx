#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace em2::net {
// Native local Discord RPC; no bot token, OAuth secret or helper executable.
void TickDiscordIntegration();
void SetDiscordParty(std::string_view room_code,unsigned players,unsigned capacity,bool joinable);
void ClearDiscordParty(std::string_view room_code);
std::optional<std::string> TakeDiscordJoinCode();
std::string DiscordConnectionStatus();
bool CanInviteDiscordFriends();
bool InviteDiscordFriends();
}
