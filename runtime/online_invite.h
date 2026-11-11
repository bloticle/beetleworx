#pragma once
#include "online.h"
namespace em2::net {
struct Invite {
    std::string address;
    uint16_t port=0;
    std::array<uint8_t,16> session{};
    uint64_t first_tick=0;
    uint8_t input_delay=2;
    bool operator==(const Invite&) const=default;
};
// Typo checksum only: an invitation is not transport encryption/authentication.
std::optional<std::string> EncodeInvite(const Invite&);
std::optional<Invite> DecodeInvite(std::string_view);
// Retains locally measured build/content/checkpoint hashes. The invitation can
// select the session/timeline, never replace compatibility evidence.
// download_checkpoint permits a missing local save for JoinWithCheckpoint;
// that session must negotiate, download and install the host's save first.
std::optional<Identity> IdentityForInvite(const Identity& local,const Invite&,bool download_checkpoint=false);
bool NewSessionNonce(std::array<uint8_t,16>&);
bool TestOnlineInvites();
}
