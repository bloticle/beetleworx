#pragma once
namespace em2 {
struct NativeAbilityEvent {const char* alias;const char* registered;};
// Existing engine registry IDs unused by either original player graph.
// The engine's event registry is fixed-size; new graph states therefore use
// these registered names, with descriptive PC aliases at the console boundary.
inline constexpr NativeAbilityEvent kNativeAbilityEvents[]={
    {"EVENT_PC_GroundPound","EVENT_SlamAttack"},
    {"EVENT_PC_GroundPoundCancel","EVENT_AbortAttack"},
    {"EVENT_PC_DashGround","EVENT_Start_AttackCharge"},
    {"EVENT_PC_DashAir","EVENT_Start_AttackLeap"},
    {"EVENT_PC_DashStopGround","EVENT_Finish_AttackCharge"},
    {"EVENT_PC_DashStopAir","EVENT_Finish_AttackLeap"}
};
}
