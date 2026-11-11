#include "online_campaigns.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <rex/logging.h>

namespace em2::net {
namespace {
namespace fs=std::filesystem;
constexpr wchar_t kMarker[]=L".em2-host-campaign-v1";
constexpr wchar_t kLease[]=L".em2-host-campaign-lease";
constexpr size_t kNativeCapacity=0x40000;
void Require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void NoLink(const fs::path& path){
    const auto attributes=GetFileAttributesW(path.c_str());
    Require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Online campaign contains a link or an unavailable path.");
}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    HANDLE Release(){const auto h=value;value=INVALID_HANDLE_VALUE;return h;}
};
fs::path CampaignBase(const fs::path& normal){
    Require(normal.is_absolute(),"The normal save directory must be absolute.");
    const auto root=fs::canonical(normal);
    return root.parent_path()/"online_sessions";
}
fs::path Candidate(const fs::path& normal,const fs::path& session){
    Require(session.is_absolute(),"The retained session directory must be absolute.");
    const auto base=CampaignBase(normal);NoLink(base);NoLink(session);
    const auto canonical=fs::canonical(session);
    const auto name=canonical.filename().string();
    Require(canonical.parent_path()==fs::canonical(base) && name.starts_with("native-") && name.size()<=96,
        "Select a retained host campaign from this game's online_sessions directory.");
    return canonical;
}
std::string Hex(std::span<const uint8_t> bytes){
    constexpr char digits[]="0123456789abcdef";std::string text; text.reserve(bytes.size()*2);
    for(auto value:bytes){text+=digits[value>>4];text+=digits[value&15];}return text;
}
bool HexText(std::string_view text,size_t bytes){
    return text.size()==bytes*2 && std::all_of(text.begin(),text.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');});
}
struct Header {std::array<uint8_t,16> bytes{};uint64_t modified=0;};
Header ReadHeader(const fs::path& title){
    const auto resolved=ResolveCampaignSaveSource(title);Require(bool(resolved.root),"The retained host campaign is missing save.dat.");
    const auto file=*resolved.root/"00000001"/"SAVEGAME"/"save.dat";NoLink(file);
    Handle handle(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    Require(handle.value!=INVALID_HANDLE_VALUE,"The retained campaign is busy or cannot be read.");
    BY_HANDLE_FILE_INFORMATION info{};
    Require(GetFileInformationByHandle(handle.value,&info)!=0 && !info.nFileSizeHigh && info.nFileSizeLow>=32 && info.nFileSizeLow<=kNativeCapacity,
        "The retained campaign is truncated or exceeds the native archive capacity.");
    Header header;DWORD read=0;
    Require(ReadFile(handle.value,header.bytes.data(),DWORD(header.bytes.size()),&read,nullptr)!=0 && read==header.bytes.size(),"The retained campaign header cannot be read.");
    Require(std::memcmp(header.bytes.data()+4,"GAME",4)==0,"The retained campaign is not a supported native GAME archive.");
    header.modified=(uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
    return header;
}
std::array<std::string,5> ReadMarker(const fs::path& session){
    const auto path=session/kMarker;NoLink(path);
    Require(fs::file_size(path)<=512,"The retained campaign marker is invalid.");
    std::ifstream input(path,std::ios::binary);std::array<std::string,5> lines;
    for(auto& line:lines)Require(bool(std::getline(input,line)),"The retained campaign marker is incomplete.");
    std::string extra;Require(!std::getline(input,extra),"The retained campaign marker has unexpected data.");
    Require(lines[0]=="EM2-NATIVE-VERIFIED-HOST-1" && lines[1]=="42560816" && HexText(lines[2],16) &&
        HexText(lines[3],32) && HexText(lines[4],16),"The retained campaign marker is unsupported.");
    return lines;
}
void CheckIdle(const fs::path& session){
    const auto path=session/kLease;NoLink(path);
    Handle lease(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    Require(lease.value!=INVALID_HANDLE_VALUE,"That online campaign is still open in another game. Close its session first.");
}
std::string Label(uint64_t modified){
    FILETIME file{DWORD(modified),DWORD(modified>>32)},local{};SYSTEMTIME time{};
    if(!FileTimeToLocalFileTime(&file,&local) || !FileTimeToSystemTime(&local,&time))return "Retained online campaign";
    char text[80];std::snprintf(text,sizeof(text),"Online campaign - %04u-%02u-%02u %02u:%02u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute);
    return text;
}
RetainedHostCampaign Inspect(const fs::path& normal,const fs::path& session,std::stop_token stop){
    Require(!stop.stop_requested(),"Online campaign discovery cancelled.");
    const auto root=Candidate(normal,session);const auto marker=ReadMarker(root);CheckIdle(root);
    const auto header=ReadHeader(root/"42560816");
    Require(Hex(header.bytes)==marker[4],"The retained campaign's native archive format has changed or its header is damaged.");
    const auto packed=PackCheckpoint(root/"42560816",stop);Require(bool(packed.value),"The retained campaign checkpoint is incomplete, busy or unsafe.");
    return {root,Label(header.modified),header.modified};
}
HostCampaignRecord Publish(const fs::path& normal,const fs::path& session,const Identity& identity){
    try{
        const auto root=Candidate(normal,session);const auto header=ReadHeader(root/"42560816");
        const auto marker=root/kMarker,lease_path=root/kLease;
        Require(!fs::exists(marker) && !fs::exists(lease_path),"That retained campaign is already registered.");
        Handle lease(CreateFileW(lease_path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        Require(lease.value!=INVALID_HANDLE_VALUE,"Cannot reserve the online campaign's active-session lease.");
        const auto body=std::string("EM2-NATIVE-VERIFIED-HOST-1\n42560816\n")+Hex(identity.session)+'\n'+Hex(identity.checkpoint)+'\n'+Hex(header.bytes)+'\n';
        Handle file(CreateFileW(marker.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        Require(file.value!=INVALID_HANDLE_VALUE,"Cannot record the resumable host campaign.");
        DWORD written=0;Require(WriteFile(file.value,body.data(),DWORD(body.size()),&written,nullptr)!=0 && written==body.size() && FlushFileBuffers(file.value),
            "Cannot finish recording the resumable host campaign.");
        return {std::make_unique<HostCampaignLease>(lease.Release()),{}};
    }catch(const std::exception& error){return {{},error.what()};}
}
}
HostCampaignLease::~HostCampaignLease(){if(handle_ && handle_!=INVALID_HANDLE_VALUE)CloseHandle(static_cast<HANDLE>(handle_));}
HostCampaignRecord RecordValidatedNativeHostCampaign(const fs::path& normal,const fs::path& session,const Session& ready){
    if(ready.state()!=State::Ready || ready.local_player()!=0 || ready.identity().startup_policy!=StartupPolicy::NativeMenu ||
        ready.identity().initialization_policy!=InitializationPolicy::SharedNativeV1)
        return {{},"Only a ready native host session may register a resumable campaign."};
    return Publish(normal,session,ready.identity());
}
HostCampaignCatalog FindRetainedHostCampaigns(const fs::path& normal,std::stop_token stop){
    HostCampaignCatalog catalog;
    try{
        const auto base=CampaignBase(normal);if(!fs::exists(base))return catalog;NoLink(base);
        size_t inspected=0;
        for(const auto& entry:fs::directory_iterator(base)){
            Require(!stop.stop_requested(),"Online campaign discovery cancelled.");
            if(++inspected>4096 || catalog.campaigns.size()>=64){catalog.truncated=true;break;}
            if(!entry.path().filename().string().starts_with("native-"))continue;
            try{catalog.campaigns.push_back(Inspect(normal,entry.path(),stop));}catch(const std::exception&){if(stop.stop_requested())throw;}
        }
        std::sort(catalog.campaigns.begin(),catalog.campaigns.end(),[](const auto& a,const auto& b){return a.saved_time!=b.saved_time?a.saved_time>b.saved_time:a.user_data<b.user_data;});
    }catch(const std::exception& error){catalog.error=error.what();}
    return catalog;
}
CampaignSaveSourceResult ValidateRetainedHostCampaign(const fs::path& normal,const fs::path& session,std::stop_token stop){
    try{const auto candidate=Inspect(normal,session,stop);return {candidate.user_data/"42560816",{}};}
    catch(const std::exception& error){return {{},error.what()};}
}
bool TestRetainedHostCampaigns(const fs::path& fixture){
    auto check=[](bool value,const char* reason){if(!value)REXLOG_ERROR("Retained host campaign test: {}",reason);return value;};
    try{
        const auto base=fs::absolute(fixture),normal=base/"user_data",sessions=base/"online_sessions";
        fs::create_directories(normal);fs::create_directories(sessions);
        std::array<uint8_t,80> bytes{};std::memcpy(bytes.data()+4,"GAME",4);bytes[11]=22;bytes[40]=17;
        auto write=[&](const fs::path& root){
            const auto directory=root/"42560816"/"00000001"/"SAVEGAME";fs::create_directories(directory);
            std::ofstream file(directory/"save.dat",std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        };
        const auto first=sessions/"native-first",second=sessions/"native-second",client_only=sessions/"native-unmarked-client";
        write(first);write(second);write(client_only);
        Session idle;
        if(!check(!RecordValidatedNativeHostCampaign(normal,first,idle).lease && !fs::exists(first/kMarker),"idle session cannot publish host authority"))return false;
        Identity identity;identity.build[0]=1;identity.content[0]=2;identity.session[0]=3;
        identity.startup_policy=StartupPolicy::NativeMenu;identity.initialization_policy=InitializationPolicy::SharedNativeV1;
        const auto packed=PackCheckpoint(first/"42560816");if(!packed.value)return false;identity.checkpoint=packed.value->identity;
        Session host,client;
        if(!check(host.Host(identity,"127.0.0.1") && client.Join(identity,"127.0.0.1",host.port()),"host authority fixture opens loopback sockets"))return false;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        do{host.Poll();client.Poll();if(host.state()==State::Ready && client.state()==State::Ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        while(std::chrono::steady_clock::now()<deadline);
        if(!check(host.state()==State::Ready && client.state()==State::Ready && !RecordValidatedNativeHostCampaign(normal,client_only,client).lease &&
            !fs::exists(client_only/kMarker),"connected client cannot publish itself as a retained host campaign"))return false;
        auto receipt=RecordValidatedNativeHostCampaign(normal,first,host);
        if(!check(bool(receipt.lease) && receipt.error.empty() && FindRetainedHostCampaigns(normal).campaigns.empty(),"real exclusive lease hides an active host campaign"))return false;
        if(!check(!ValidateRetainedHostCampaign(normal,first).root &&
            !RecordValidatedNativeHostCampaign(normal,first,host).lease,
            "an active host archive cannot be resumed or registered a second time"))return false;
        receipt.lease.reset();
        auto catalog=FindRetainedHostCampaigns(normal);
        if(!check(catalog.campaigns.size()==1 && catalog.campaigns[0].user_data==fs::canonical(first) &&
            ValidateRetainedHostCampaign(normal,first).root==fs::canonical(first)/"42560816", "released marked host is discoverable; unmarked/client folders remain hidden"))return false;
        if(!check(!ValidateRetainedHostCampaign(normal,normal).root && !ValidateRetainedHostCampaign(normal,base/"other-session").root,
            "normal saves and paths outside this archive base cannot be selected as retained sessions"))return false;
        bytes[60]^=0x31;write(first);
        const auto progressed=PackCheckpoint(first/"42560816");
        if(!check(progressed.value && progressed.value->identity!=identity.checkpoint && ValidateRetainedHostCampaign(normal,first).root,
            "a changed save payload remains eligible rather than being frozen to the initial checkpoint hash"))return false;
        bytes[4]='B';write(first);
        if(!check(!ValidateRetainedHostCampaign(normal,first).root && FindRetainedHostCampaigns(normal).campaigns.empty(),"damaged native header is rejected"))return false;
        bytes[4]='G';write(first);
        auto other=RecordValidatedNativeHostCampaign(normal,second,host);if(!other.lease)return false;other.lease.reset();
        const auto second_file=second/"42560816"/"00000001"/"SAVEGAME"/"save.dat";
        fs::last_write_time(second_file,fs::last_write_time(first/"42560816"/"00000001"/"SAVEGAME"/"save.dat")+std::chrono::seconds(5));
        catalog=FindRetainedHostCampaigns(normal);
        if(!check(catalog.campaigns.size()==2 && catalog.campaigns[0].user_data==fs::canonical(second),"newest native autosave is selected first"))return false;
        {std::ofstream damaged(second/kMarker,std::ios::app);damaged<<"unexpected-data\n";}
        catalog=FindRetainedHostCampaigns(normal);
        if(!check(catalog.campaigns.size()==1 && catalog.campaigns[0].user_data==fs::canonical(first),"malformed marker does not become selectable"))return false;
        std::stop_source cancel;cancel.request_stop();
        if(!check(!FindRetainedHostCampaigns(normal,cancel.get_token()).error.empty(),"worker cancellation is reported"))return false;
        REXLOG_INFO("Retained host campaign fixtures passed: real loopback host authority/exclusive leases, bounded checkpoint/header validation, later progress, scope guards, ordering, malformed markers and cancellation; native decode remains a runtime requirement");return true;
    }catch(const std::exception& error){REXLOG_ERROR("Retained host campaign fixture exception: {}",error.what());return false;}
}
}
