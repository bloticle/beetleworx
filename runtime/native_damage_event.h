#pragma once
#include "online_damage_event.h"
struct PPCContext;
namespace em2 {
// Before native Damage dispatch, on its native owner thread. Read-only and
// transactional, with a registry lifetime fence. Collision child keys are
// scalar asset identifiers; collidable pointers are never transmitted.
bool CaptureNativeDamageEvent(uint8_t* base,uint32_t event,net::DamageEvent&,const char*& error);
// Exact native reconstruction and ordered dispatch. Native arrays and tag
// references are allocated/freed through the original engine. Missing entities
// fail rather than becoming remotely supplied pointers or guessed handles.
bool ReplayNativeDamageEvents(PPCContext&,uint8_t*,std::span<const net::DamageEvent>,const char*& error);
bool TestNativeDamageEvent();
}
