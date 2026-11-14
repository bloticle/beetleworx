#pragma once
#include <filesystem>
#include <string>
namespace em2::net {
struct VoicePreferences {
    int mode=0,volume=4,key=0,positioning=1,range=2;
    std::string input,output;
    bool operator==(const VoicePreferences&)const=default;
};
bool ValidVoiceDeviceName(const std::string&);
VoicePreferences ReadVoicePreferences(const std::filesystem::path&);
bool WriteVoicePreferences(const std::filesystem::path&,const VoicePreferences&,std::string& error);
bool TestOnlineVoicePreferences(const std::filesystem::path&);
}
