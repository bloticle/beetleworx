#pragma once
#include "online_button_actions.h"
#include <memory>
namespace em2::net {
class RetailButtonReceiver {
public:
    RetailButtonReceiver();~RetailButtonReceiver();
    RetailButtonReceiver(const RetailButtonReceiver&)=delete;
    RetailButtonReceiver& operator=(const RetailButtonReceiver&)=delete;
private:
    struct Impl;std::unique_ptr<Impl> impl_;friend struct RetailButtonAccess;
};
// Receiver ownership is independent of the local physical producer. Capture
// each retail slot's actual discovered definitions at the admitted boundary.
bool ArmRetailButtons(RetailButtonReceiver&,unsigned slot,const NativeTriggerBinding&,PPCContext&,uint8_t*);
bool ResetRetailButtons(RetailButtonReceiver&);
// Complete read-only preflight. A stale native owner/definition retires the
// receiver; a malformed packet leaves it usable for a valid retry. No dispatch.
bool PrepareRetailButtons(RetailButtonReceiver&,const RoomMovement&,PPCContext&,uint8_t*,std::vector<NativeActorAction>&);
bool TestRetailButtonReceivers(PPCContext&,uint8_t*);
}
