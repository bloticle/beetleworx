#pragma once
#include <cstdint>
#include <filesystem>
#include "online_voice_audio.h"
namespace em2::net {
class Room;
struct OnlineVoiceScene {
    std::array<uint8_t,16> session{};
    uint64_t world=0,local_peer=0;
    VoicePosition listener;
    std::array<float,3> right{1,0,0};
    std::array<VoicePosition,16> players{};
};
// Native render owner publishes copied, validated values only. No native
// addresses or callbacks are stored; unavailable/stale scenes mute proximity.
bool PublishOnlineVoiceScene(const OnlineVoiceScene&);
void ClearOnlineVoiceScene();
// Room owner only; device/codec calls run on an independent media worker.
void PumpOnlineVoice(Room*);
void LoadOnlineVoiceSettings(const std::filesystem::path& directory);
bool NativeOnlineVoiceOption(int id,int choice,int& result);
bool OnlineVoiceKey(unsigned key,bool down);
uint16_t OnlineVoicePad(uint16_t buttons);
void ReleaseOnlineVoiceInput();
void SetOnlineVoiceFocused(bool);
bool TestOnlineVoiceRuntime();
bool TestOnlineVoiceSettings(const std::filesystem::path&);
}
