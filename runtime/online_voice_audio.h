#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace em2::net {
inline constexpr unsigned kVoiceRate=48000,kVoiceSamples=960,kVoicePacketBytes=400;
using VoiceTime=std::chrono::steady_clock::time_point;
using VoicePcm=std::array<float,kVoiceSamples>;
struct VoicePacket {
    // Monotonic 20-ms source timestamp, including time spent not transmitting.
    // It must not restart when push-to-talk is released/repressed.
    uint64_t sequence=0;
    uint16_t size=0;
    std::array<uint8_t,kVoicePacketBytes> data{};
};
struct VoiceDevices {std::vector<std::string> input,output;std::string error;};
// Metadata enumeration only, on a worker; never opens a microphone/speaker.
VoiceDevices EnumerateVoiceDevices();
class VoiceEncoder {
    struct State;std::unique_ptr<State> state_;
public:
    VoiceEncoder();~VoiceEncoder();
    VoiceEncoder(const VoiceEncoder&)=delete;
    VoiceEncoder& operator=(const VoiceEncoder&)=delete;
    bool Encode(uint64_t,std::span<const float>,VoicePacket&);
    void Reset();
};
struct VoicePosition {bool valid=false;uint64_t world=0;std::array<float,3> point{};};
enum class VoiceMode {Party,Proximity};
struct VoiceMixSettings {
    VoiceMode mode=VoiceMode::Party;
    VoicePosition listener;
    std::array<float,3> right{1,0,0};
    float near_distance=2,far_distance=30,volume=1;
};
struct VoicePeerMix {bool muted=false;float volume=1;VoicePosition position;};
std::array<float,2> VoiceSpatialGain(const VoiceMixSettings&,const VoicePosition&);
// Single owner, bounded eight-packet jitter queue per peer, sixteen decoders.
// Device wrapper serializes Submit/configuration with its audio callback.
class VoiceMixer {
    struct State;std::unique_ptr<State> state_;
public:
    VoiceMixer();~VoiceMixer();
    VoiceMixer(const VoiceMixer&)=delete;
    VoiceMixer& operator=(const VoiceMixer&)=delete;
    bool Submit(unsigned peer,const VoicePacket&,VoiceTime);
    bool Configure(const VoiceMixSettings&);
    bool ConfigureScene(const VoiceMixSettings&,const std::array<VoicePeerMix,16>&);
    bool SetPeer(unsigned peer,bool muted,float volume,const VoicePosition&);
    void ResetPeer(unsigned peer);
    void Render(VoiceTime,std::span<float> interleaved_stereo);
    size_t Pending(unsigned peer)const;
};
// Explicit device lifetime; construction never opens a microphone or speaker.
// Capture starts only after OpenCapture. PollCapture belongs on a media worker,
// not the simulation or SDL callback thread. All device errors are returned.
class VoiceAudio {
    friend bool TestOnlineVoiceAudio();
    struct State;std::unique_ptr<State> state_;
public:
    VoiceAudio();~VoiceAudio();
    VoiceAudio(const VoiceAudio&)=delete;
    VoiceAudio& operator=(const VoiceAudio&)=delete;
    bool OpenCapture(std::string device,std::string& error);
    bool OpenPlayback(std::string device,std::string& error);
    void CloseCapture();
    // Drop queued/partial PCM and reset codec redundancy at a PTT boundary.
    void FlushCapture();
    void ClosePlayback();
    bool PollCapture(VoicePacket&);
    bool Submit(unsigned peer,const VoicePacket&,VoiceTime);
    bool Configure(const VoiceMixSettings&);
    bool ConfigureScene(const VoiceMixSettings&,const std::array<VoicePeerMix,16>&);
    bool SetPeer(unsigned peer,bool muted,float volume,const VoicePosition&);
    uint64_t DroppedCaptureFrames()const;
};
bool TestOnlineVoiceAudio();
}
