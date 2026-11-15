#pragma once
#include "online_damage_event.h"
namespace em2 {
// Before native Damage dispatch, on its native owner thread. Read-only and
// transactional, with a registry lifetime fence. Unsupported auxiliary payloads
// are reported rather than copied as potentially process-local words.
bool CaptureNativeDamageEvent(uint8_t* base,uint32_t event,net::DamageEvent&,const char*& error);
bool TestNativeDamageEvent();
}
