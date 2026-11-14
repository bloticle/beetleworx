#include "online_voice_devices.h"
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <stdexcept>
namespace em2::net {
struct VoiceDeviceCatalog::Impl {
    mutable std::mutex mutex;VoiceDeviceView view;std::function<VoiceDevices()> scan;
    std::jthread thread;
};
VoiceDeviceCatalog::VoiceDeviceCatalog(std::function<VoiceDevices()> scan):impl_(std::make_unique<Impl>()){impl_->scan=std::move(scan);}
VoiceDeviceCatalog::~VoiceDeviceCatalog()=default;
void VoiceDeviceCatalog::Refresh(bool force){
    auto& s=*impl_;std::lock_guard lock(s.mutex);if(s.view.busy || (s.view.scanned && !force))return;
    if(s.thread.joinable())s.thread.join();s.view.busy=true;s.view.scanned=true;
    try{s.thread=std::jthread([&s]{
        VoiceDevices result;try{result=s.scan();}catch(const std::exception& e){result.error=std::string(e.what()).substr(0,160);}catch(...){result.error="Audio device scan failed.";}
        std::lock_guard lock(s.mutex);s.view.devices=std::move(result);s.view.busy=false;
    });}catch(...){s.view.busy=false;s.view.devices.error="Could not start audio device scan.";}
}
VoiceDeviceView VoiceDeviceCatalog::Snapshot()const{std::lock_guard lock(impl_->mutex);return impl_->view;}
std::string CycleVoiceDevice(const VoiceDeviceView& view,bool capture,const std::string& current,int direction){
    if(view.busy)return current;const auto& names=capture?view.devices.input:view.devices.output;
    auto found=std::find(names.begin(),names.end(),current);if(!current.empty() && found==names.end())return {};
    const auto count=int(names.size())+1,index=current.empty()?0:int(found-names.begin())+1;
    const auto next=(index+(direction<0?-1:1)+count)%count;return next?names[size_t(next-1)]:std::string{};
}
std::string VoiceDeviceLabel(const VoiceDeviceView& view,bool capture,const std::string& current){
    if(current.empty())return "System default";const auto& names=capture?view.devices.input:view.devices.output;
    return current+(view.scanned && !view.busy && std::find(names.begin(),names.end(),current)==names.end()?" (not connected)":"");
}
bool TestOnlineVoiceDevices(){
    bool ok=true;unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value){ok=false;REXLOG_ERROR("Voice device catalog: {}",why);}};
    std::atomic<unsigned> calls{0};std::atomic<bool> release{false};const auto owner=std::this_thread::get_id();std::atomic<bool> worker{false};
    VoiceDeviceCatalog catalog([&]{worker=std::this_thread::get_id()!=owner;const auto n=++calls;while(!release.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));if(n==2)throw std::runtime_error("injected scan failure");return VoiceDevices{{"Mic A","Mic B"},{"Speakers A"},{}};});
    auto wait=[&]{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(catalog.Snapshot().busy && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(1));return !catalog.Snapshot().busy;};
    check(!catalog.Snapshot().scanned && calls==0,"construction never initializes or enumerates hardware");catalog.Refresh();
    check(catalog.Snapshot().busy,"scan returns immediately while worker is blocked");catalog.Refresh(true);release=true;
    check(wait() && calls==1 && worker,"busy scan is not duplicated and metadata work stays off caller");
    auto view=catalog.Snapshot();check(CycleVoiceDevice(view,true,"",1)=="Mic A" && CycleVoiceDevice(view,true,"Mic A",1)=="Mic B" && CycleVoiceDevice(view,true,"Mic B",1).empty(),"default/named capture cycle wraps");
    check(CycleVoiceDevice(view,false,"",-1)=="Speakers A" && CycleVoiceDevice(view,false,"Speakers A",1).empty(),"playback and backwards selection independent");
    std::reverse(view.devices.input.begin(),view.devices.input.end());check(VoiceDeviceLabel(view,true,"Mic A")=="Mic A","reordered catalog preserves selected device by name");
    check(VoiceDeviceLabel(view,true,"Removed mic").find("not connected")!=std::string::npos && CycleVoiceDevice(view,true,"Removed mic",1).empty(),"missing saved device is explicit and next selects System default");
    catalog.Refresh();check(calls==1,"ordinary query never retries a finished scan");catalog.Refresh(true);
    check(wait() && calls==2 && catalog.Snapshot().devices.error=="injected scan failure","explicit refresh contains scan exception");
    check(CycleVoiceDevice(catalog.Snapshot(),true,"",1).empty(),"default remains selectable when inventory fails");
    REXLOG_INFO("Voice device catalog {} {} checks: asynchronous metadata, bounded scan admission, stable named selection, default fallback and failure isolation; injected enumeration only",ok?"passed":"FAILED",checks);return ok;
}
}
