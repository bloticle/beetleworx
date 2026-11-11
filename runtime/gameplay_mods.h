#pragma once
#include <filesystem>
#include "controls.h"
struct PPCContext;
namespace em2 {
// Retail 8295E1A8 registers ST_PHYSICAL=2 and ST_SPIN=8.
enum class PoundDamageType : uint32_t { Physical=2,Spin=8 };
struct GameplayTuning {
    bool sprint=true,dash=true,pound=true,air_dash=true;
    bool keyboard_sprint_toggle=false,controller_sprint_toggle=true,native_animations=true,pound_damage=true,pound_knockback=true;
    bool dash_spin_animation=true;
    PoundDamageType pound_damage_type=PoundDamageType::Spin;
    float sprint_multiplier=1.6f,dash_speed=12.f,dash_duration=.2f,dash_cooldown=.65f,pound_speed=20.f;
    float pound_damage_amount=2.f,pound_radius=3.f;
};
void InitializeGameplayMods(const std::filesystem::path&);
bool GameplayModOption(int id,int command,int& result);
void UpdateGameplayModTargets(PPCContext&,uint8_t* base);
GameplayTuning GetGameplayTuning();
bool ChangeGameplayTuning(const GameplayTuning&);
bool QueuePlayerImpulse(float x,float y,float z,float decay);
bool SetPlayerSpeedOverride(float speed);
float PlayerSpeedOverride();
bool TestGameplayMods(const std::filesystem::path& directory);
}
