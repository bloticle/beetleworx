#include "online_voice_preferences.h"
#include <windows.h>
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <fstream>
#include <optional>
#include <string_view>
#include <array>
#include <sstream>

namespace em2::net {
namespace {
std::string Hex(std::string_view text){constexpr char digits[]="0123456789abcdef";std::string out;for(unsigned char c:text){out+=digits[c>>4];out+=digits[c&15];}return out;}
std::optional<std::string> Unhex(std::string_view value){
    if(value.size()>1024 || value.size()%2)return {};std::string out;
    for(size_t i=0;i<value.size();i+=2){unsigned n=0;const auto p=std::from_chars(value.data()+i,value.data()+i+2,n,16);if(p.ec!=std::errc{} || p.ptr!=value.data()+i+2)return {};out+=char(n);}
    return ValidVoiceDeviceName(out)?std::optional(out):std::nullopt;
}
bool Valid(const VoicePreferences& p){return p.mode>=0 && p.mode<4 && p.volume>=0 && p.volume<9 && p.key>=0 && p.key<3 && p.positioning>=0 && p.positioning<2 && p.range>=0 && p.range<5 && ValidVoiceDeviceName(p.input) && ValidVoiceDeviceName(p.output);}
}
bool ValidVoiceDeviceName(const std::string& name){return name.size()<=512 && std::none_of(name.begin(),name.end(),[](unsigned char c){return c<32 || c==127;});}
VoicePreferences ReadVoicePreferences(const std::filesystem::path& file){
    VoicePreferences p;std::error_code ec;const auto size=std::filesystem::file_size(file,ec);if(ec || size>4096)return p;
    std::array<char,4097> data{};std::ifstream input(file,std::ios::binary);input.read(data.data(),data.size());const auto length=input.gcount();if(length>4096)return p;
    std::istringstream stream(std::string(data.data(),size_t(length)));std::string line;
    while(std::getline(stream,line)){
        if(!line.empty() && line.back()=='\r')line.pop_back();
        const auto at=line.find('=');if(at==std::string::npos)continue;const auto key=line.substr(0,at);const std::string_view value(line.data()+at+1,line.size()-at-1);
        if(key=="input" || key=="output"){if(auto decoded=Unhex(value))(key=="input"?p.input:p.output)=*decoded;continue;}
        int n=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),n);if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size())continue;
        if(key=="mode" && n>=0 && n<4)p.mode=n;
        else if(key=="volume" && n>=0 && n<9)p.volume=n;
        else if(key=="key" && n>=0 && n<3)p.key=n;
        else if(key=="positioning" && n>=0 && n<2)p.positioning=n;
        else if(key=="range" && n>=0 && n<5)p.range=n;
    }
    return p;
}
bool WriteVoicePreferences(const std::filesystem::path& file,const VoicePreferences& p,std::string& error){
    error.clear();if(!Valid(p)){error="Invalid voice settings.";return false;}if(file.empty())return true;
    static std::atomic<uint64_t> serial{0};auto temporary=file;temporary+=L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++serial);
    {
        std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);
        stream<<"mode="<<p.mode<<"\nvolume="<<p.volume<<"\nkey="<<p.key<<"\npositioning="<<p.positioning<<"\nrange="<<p.range<<"\ninput="<<Hex(p.input)<<"\noutput="<<Hex(p.output)<<"\n";
        stream.flush();if(!stream){error="Could not write voice settings.";stream.close();std::error_code ec;std::filesystem::remove(temporary,ec);return false;}
    }
    if(!MoveFileExW(temporary.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){error="Could not replace voice settings (Windows error "+std::to_string(GetLastError())+").";std::error_code ec;std::filesystem::remove(temporary,ec);return false;}return true;
}
bool TestOnlineVoicePreferences(const std::filesystem::path& root){
    std::error_code ec;if(!std::filesystem::create_directory(root,ec) || ec)return false;
    const auto file=root/"voice-settings.ini";bool ok=true;unsigned checks=0;auto check=[&](bool v,const char* why){++checks;if(!v){ok=false;REXLOG_ERROR("Voice preferences: {}",why);}};
    const VoicePreferences defaults;check(ReadVoicePreferences(file)==defaults,"missing file defaults to voice Off");
    VoicePreferences p{3,8,2,1,4,"USB \xE3\x83\x9E\xE3\x82\xA4\xE3\x82\xAF","Speakers = A"};std::string error;
    check(WriteVoicePreferences(file,p,error) && ReadVoicePreferences(file)==p,"all choices and Unicode device names round trip without ordinal dependence");
    auto invalid=p;invalid.input=std::string("bad\0name",8);check(!WriteVoicePreferences(file,invalid,error) && ReadVoicePreferences(file)==p,"invalid device cannot replace prior valid file");
    {std::ofstream out(file);out<<"mode=3junk\nvolume=999\nkey=-1\npositioning=2\nrange=5\ninput=00\noutput=zz\n";}
    check(ReadVoicePreferences(file)==defaults,"malformed individual fields cannot enable microphone or enter native device APIs");
    {std::ofstream out(file);out<<std::string(5000,'x');}check(ReadVoicePreferences(file)==defaults,"oversized settings file rejected before parsing");
    check(WriteVoicePreferences(file,defaults,error) && ReadVoicePreferences(file)==defaults,"atomic replacement restores defaults");
    check(!WriteVoicePreferences(root/"missing"/"voice.ini",p,error),"unwritable target returns actionable failure");
    std::filesystem::remove(file,ec);std::filesystem::remove(root,ec);
    REXLOG_INFO("Voice preferences {} {} checks: bounded strict parsing, all settings and named devices, Unicode, atomic replacement and failure preservation",ok?"passed":"FAILED",checks);return ok;
}
}
