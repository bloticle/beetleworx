#pragma once
#include "online_voice_audio.h"
#include <functional>
#include <memory>
namespace em2::net {
struct VoiceDeviceView {VoiceDevices devices;bool scanned=false,busy=false;};
class VoiceDeviceCatalog {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit VoiceDeviceCatalog(std::function<VoiceDevices()> scan=EnumerateVoiceDevices);
    ~VoiceDeviceCatalog();
    void Refresh(bool force=false);
    VoiceDeviceView Snapshot()const;
};
std::string CycleVoiceDevice(const VoiceDeviceView&,bool capture,const std::string& current,int direction);
std::string VoiceDeviceLabel(const VoiceDeviceView&,bool capture,const std::string& current);
bool TestOnlineVoiceDevices();
}
