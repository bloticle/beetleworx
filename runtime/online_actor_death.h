#pragma once
#include "online_actor_input.h"
#include <cstdint>
struct PPCContext;
namespace em2::net {
struct OwnedActorDeathTicket {
    uint64_t consumer_epoch=0,sequence=0;
    OwnedActorInput actor{};
};
enum class OwnedActorDeathState { Current, Retired };
// Explicit lifecycle consumer opt-in. Anchor must resolve to a registered extra
// on the game thread in this registry. Nothing arms the queue automatically.
bool ArmOwnedActorDeaths(uint32_t registry,uint64_t consumer_epoch,uint64_t anchor_peer,PPCContext&,uint8_t* base);
// Stops interception; pending tickets remain until the same consumer confirms.
// The consumer must complete native actor removal OR world teardown before
// confirming retired ownership. Never unregister a still-live listener merely
// to make a ticket disappear. Capacity is bounded by the 16 registered extras;
// finish prior incarnations before admitting replacement actors for a peer.
bool DisarmOwnedActorDeaths(uint64_t consumer_epoch);
// Post-tick coordinator ONLY. Retired means world/handle/controller ownership
// no longer matches; never use its captured addresses as live cleanup targets.
// Peek never calls cleanup, dispatches native input or removes the ticket.
bool PeekOwnedActorDeath(uint64_t consumer_epoch,PPCContext&,uint8_t* base,OwnedActorDeathTicket&,OwnedActorDeathState&);
bool ConfirmOwnedActorDeath(uint64_t consumer_epoch,const OwnedActorDeathTicket&);
bool TestOwnedActorDeaths(PPCContext&,uint8_t* base,uint64_t first_peer,uint64_t second_peer);
}
