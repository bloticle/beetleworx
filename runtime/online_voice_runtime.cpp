#include "online_voice_runtime.h"
#include "online_voice_audio.h"
#include "online_voice_preferences.h"
#include "online_voice_devices.h"
#include "online_room.h"
#include "allocation_trace.h"
#include "controls.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <limits>
#include <windows.h>
#include <immintrin.h>

REXCVAR_DECLARE(bool,em2_probe_online_voice);
namespace em2::net {
namespace {
using DeviceName=std::array<char,513>;
DeviceName Name(std::string_view value){DeviceName name{};std::copy_n(value.data(),std::min(value.size(),size_t(512)),name.data());return name;}
struct Config {uint64_t epoch=0,session=0,world=0,local_peer=0;std::array<uint8_t,16> token{};DeviceName input{},output{};bool enabled=false,capture=false,talk=false,proximity=false;uint16_t roster=0;float volume=1,far_distance=30;bool operator==(const Config&)const=default;};
struct SceneCopy {OnlineVoiceScene scene;VoiceTime at{};uint64_t revision=0;};
std::mutex scene_mutex;SceneCopy published_scene;
bool ValidScene(const OnlineVoiceScene& scene){
    if(!scene.world || scene.world==UINT64_MAX || scene.local_peer<1 || scene.local_peer>16 ||
       !std::any_of(scene.session.begin(),scene.session.end(),[](auto byte){return byte!=0;}) ||
       !scene.listener.valid || scene.listener.world!=scene.world)return false;
    auto finite=[](const auto& values){return std::all_of(values.begin(),values.end(),[](float value){return std::isfinite(value);});};
    if(!finite(scene.listener.point) || !finite(scene.right))return false;
    double length=0;for(float value:scene.right)length+=double(value)*value;if(length<.9 || length>1.1)return false;
    for(const auto& peer:scene.players)if(!finite(peer.point) || (peer.valid && peer.world!=scene.world))return false;
    const auto& local=scene.players[scene.local_peer-1];return local.valid && local.world==scene.world && local.point==scene.listener.point;
}
SceneCopy SceneFor(const Config& c,VoiceTime now){
    if(!c.proximity)return {};
    SceneCopy copy;{std::lock_guard lock(scene_mutex);copy=published_scene;}
    if(!ValidScene(copy.scene) || copy.at>now || now-copy.at>std::chrono::milliseconds(250) ||
       copy.scene.world!=c.world || copy.scene.local_peer!=c.local_peer || copy.scene.session!=c.token)return {};
    return copy;
}
struct Audio {
    virtual ~Audio()=default;
    virtual bool Playback(const char*,std::string&)=0;
    virtual bool Capture(const char*,std::string&)=0;
    virtual void CloseCapture()=0;
    virtual void FlushCapture()=0;
    virtual bool Poll(VoicePacket&)=0;
    virtual void Submit(unsigned,const VoicePacket&,VoiceTime)=0;
    virtual void Configure(const Config&,const OnlineVoiceScene&)=0;
    virtual void Scene(const OnlineVoiceScene&){}
};
struct Devices final:Audio {
    VoiceAudio audio;
    Config config;OnlineVoiceScene scene;
    bool Playback(const char* name,std::string& e)override{return audio.OpenPlayback(name,e);}
    bool Capture(const char* name,std::string& e)override{return audio.OpenCapture(name,e);}
    void CloseCapture()override{audio.CloseCapture();}
    void FlushCapture()override{audio.FlushCapture();}
    bool Poll(VoicePacket& p)override{return audio.PollCapture(p);}
    void Submit(unsigned peer,const VoicePacket& p,VoiceTime now)override{audio.Submit(peer,p,now);}
    void Apply(){
        VoiceMixSettings settings;settings.volume=config.volume;settings.mode=config.proximity?VoiceMode::Proximity:VoiceMode::Party;
        settings.far_distance=config.far_distance;settings.near_distance=config.far_distance*.1f;
        if(scene.world){settings.listener=scene.listener;settings.right=scene.right;}
        std::array<VoicePeerMix,16> peers;
        for(unsigned i=0;i<peers.size();++i){peers[i].position=scene.players[i];peers[i].muted=!(config.roster&(1u<<i)) || (config.proximity && !scene.players[i].valid);}
        audio.ConfigureScene(settings,peers);
    }
    void Configure(const Config& c,const OnlineVoiceScene& s)override{config=c;scene=s;Apply();}
    void Scene(const OnlineVoiceScene& s)override{scene=s;Apply();}
};
// One owner and one media worker exchange bounded copied values. No audio API
// or device wait runs while this mutex is held; guest pointers never enter it.
class MediaWorker {
    struct Captured {uint64_t epoch;VoicePacket packet;};
    struct Incoming {uint64_t epoch;unsigned peer;VoicePacket packet;VoiceTime at;};
    std::mutex mutex;
    Config requested;std::deque<Incoming> incoming;std::deque<Captured> outgoing;
    std::string error;bool playback=false,microphone=false,spatial_wait=false;uint64_t applied=0,dropped=0;
    std::function<std::unique_ptr<Audio>()> factory;
    std::function<void(Config&)> restrict;
    // Declared last: joining precedes destruction of all shared state.
    std::jthread thread;
    void Run(std::stop_token stop){
        std::unique_ptr<Audio> audio;Config prior;uint64_t failed=0,scene_revision=0;std::string playback_error,capture_error;
        while(!stop.stop_requested()){
            Config c;std::deque<Incoming> received;
            {std::lock_guard lock(mutex);c=requested;received.swap(incoming);}
            try{
                restrict(c);
                const auto spatial=SceneFor(c,std::chrono::steady_clock::now());
                {std::lock_guard lock(mutex);spatial_wait=c.enabled && c.proximity && !spatial.scene.world;}
                if(c.epoch!=prior.epoch || c.enabled!=prior.enabled || c.capture!=prior.capture || c.talk!=prior.talk){
                    // Retire old queued speech before reconfiguration or a new
                    // Room. Capture epochs also fence queued PTT releases.
                    {std::lock_guard lock(mutex);outgoing.clear();if(!c.enabled || !prior.enabled || c.session!=prior.session)error.clear();}
                    const bool new_session=c.session!=prior.session;
                    if(!c.enabled || !prior.enabled || new_session){playback_error.clear();capture_error.clear();}
                    if(!c.enabled || new_session){audio.reset();playback=false;microphone=false;}
                    if(c.enabled && (!prior.enabled || new_session))audio=factory();
                    if(audio && c.enabled && (!prior.enabled || new_session || c.output!=prior.output)){std::string why;playback=audio->Playback(c.output.data(),why);playback_error=playback?"":"Voice playback unavailable: "+why.substr(0,160);}
                    if(audio){
                        audio->Configure(c,spatial.scene);scene_revision=spatial.revision;
                        if(c.capture && (!prior.capture || new_session || c.input!=prior.input)){std::string why;microphone=audio->Capture(c.input.data(),why);capture_error=microphone?"":"Microphone unavailable: "+why.substr(0,160);}
                        else if(!c.capture && prior.capture){audio->CloseCapture();microphone=false;capture_error.clear();}
                        if(microphone && c.talk && !prior.talk)audio->FlushCapture();
                        {std::lock_guard lock(mutex);error=playback_error.empty()?capture_error:playback_error;}
                    }
                    prior=c;
                }
                if(audio && c.enabled && scene_revision!=spatial.revision){audio->Scene(spatial.scene);scene_revision=spatial.revision;}
                if(audio && c.enabled){
                    if(playback)for(const auto& item:received)if(item.epoch==c.epoch && (c.roster&(1u<<(item.peer-1))))audio->Submit(item.peer,item.packet,item.at);
                    VoicePacket packet;
                    for(unsigned i=0;microphone && i<8 && audio->Poll(packet);++i){
                        // Drain while PTT is released; never transmit pre-press
                        // buffered audio or retain it for a later press.
                        if(c.talk){std::lock_guard lock(mutex);if(requested.epoch==c.epoch && outgoing.size()<8)outgoing.push_back({c.epoch,packet});else ++dropped;}
                    }
                }
            }catch(const std::exception& e){audio.reset();playback=false;microphone=false;failed=c.epoch;std::lock_guard lock(mutex);error="Voice stopped: "+std::string(e.what()).substr(0,160);}
            catch(...){audio.reset();playback=false;microphone=false;failed=c.epoch;std::lock_guard lock(mutex);error="Voice stopped after an audio failure.";}
            {std::lock_guard lock(mutex);applied=c.epoch;}
            // A failed configuration is not retried every poll. A deliberate
            // mode change (Off then On) creates the next bounded attempt.
            if(failed==c.epoch)prior=c;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
public:
    explicit MediaWorker(std::function<std::unique_ptr<Audio>()> f=[] {return std::make_unique<Devices>();},std::function<void(Config&)> p=[](Config&){}):factory(std::move(f)),restrict(std::move(p)),thread([this](std::stop_token s){Run(s);}){}
    void Configure(Config c){std::lock_guard lock(mutex);if(c==requested)return;requested=c;incoming.clear();outgoing.clear();}
    bool Submit(uint64_t epoch,unsigned peer,const VoicePacket& packet,VoiceTime at){
        std::lock_guard lock(mutex);if(peer<1 || peer>16 || epoch!=requested.epoch || !requested.enabled || !(requested.roster&(1u<<(peer-1))) || !packet.size || packet.size>packet.data.size() || incoming.size()==64){++dropped;return false;}
        incoming.push_back({epoch,peer,packet,at});return true;
    }
    bool Take(uint64_t epoch,VoicePacket& out){std::lock_guard lock(mutex);while(!outgoing.empty()){auto item=outgoing.front();outgoing.pop_front();if(item.epoch==epoch && requested.epoch==epoch && requested.talk){out=item.packet;return true;}}return false;}
    std::string Status(){std::lock_guard lock(mutex);return !error.empty()?error:spatial_wait?"Waiting for player positions; proximity voice muted.":"";}
    uint64_t Applied(){std::lock_guard lock(mutex);return applied;}
};
std::atomic<int> mode{0},volume{4},key_index{0},spatial_mode{0},range_index{2};
std::mutex preferences_mutex;std::filesystem::path preferences_file;DeviceName input_device{},output_device{};std::string preferences_error;
VoiceDeviceCatalog& Catalog(){static VoiceDeviceCatalog catalog;return catalog;}
void SavePreferencesLocked(){
    VoicePreferences p{mode.load(),volume.load(),key_index.load(),spatial_mode.load(),range_index.load(),input_device.data(),output_device.data()};
    try{WriteVoicePreferences(preferences_file,p,preferences_error);}catch(...){preferences_error="Voice settings could not be saved.";}
}
constexpr float ranges[]={10,20,30,50,100};
constexpr unsigned keys[]={VK_F8,VK_CAPITAL,VK_MENU};
std::atomic<bool> key_down{false},pad_down{false},connected{false},focused{true};
std::mutex status_mutex;std::string status="Voice is off.";
bool Talking(){return focused.load() && (mode.load()==3 || (mode.load()==2 && (key_down.load() || pad_down.load())));}
}
bool PublishOnlineVoiceScene(const OnlineVoiceScene& scene){
    const bool valid=ValidScene(scene);std::lock_guard lock(scene_mutex);
    published_scene.scene=valid?scene:OnlineVoiceScene{};published_scene.at=std::chrono::steady_clock::now();++published_scene.revision;return valid;
}
void ClearOnlineVoiceScene(){std::lock_guard lock(scene_mutex);published_scene.scene={};published_scene.at={};++published_scene.revision;}
void LoadOnlineVoiceSettings(const std::filesystem::path& directory){
    std::lock_guard lock(preferences_mutex);preferences_file=directory/"voice-settings.ini";const auto p=ReadVoicePreferences(preferences_file);
    mode=p.mode;volume=p.volume;key_index=p.key;spatial_mode=p.positioning;range_index=p.range;input_device=Name(p.input);output_device=Name(p.output);preferences_error.clear();ReleaseOnlineVoiceInput();
}
void ReleaseOnlineVoiceInput(){key_down=false;pad_down=false;}
void SetOnlineVoiceFocused(bool value){focused=value;if(!value)ReleaseOnlineVoiceInput();}
bool OnlineVoiceKey(unsigned key,bool down){
    if(key!=keys[unsigned(key_index.load())])return false;
    const bool consume=connected.load() && focused.load() && mode.load()==2;
    key_down=consume && down;return consume;
}
uint16_t OnlineVoicePad(uint16_t buttons){const bool consume=connected.load() && focused.load() && mode.load()==2 && !em2::ControlBindingsOpen();pad_down=consume && (buttons&2);return consume?uint16_t(buttons&2):0;}
bool NativeOnlineVoiceOption(int id,int choice,int& result){
    result=0;
    if(id==2800 || id==2801 || id==2802 || id==2804 || id==2805){
        auto& value=id==2800?mode:id==2801?volume:id==2802?key_index:id==2804?spatial_mode:range_index;const int count=id==2800?4:id==2801?9:id==2802?3:id==2804?2:5;
        if(choice>=0 && choice<count){const bool changed=value.exchange(choice)!=choice;ReleaseOnlineVoiceInput();if(changed){std::lock_guard lock(preferences_mutex);SavePreferencesLocked();}}result=value.load();return true;
    }
    if(id==2803){static thread_local std::string message;if(choice==0){
        {std::lock_guard lock(preferences_mutex);message=preferences_error;}{std::lock_guard lock(status_mutex);if(message.empty())message=status;}
        const auto view=Catalog().Snapshot();if(view.busy)message+=" Scanning audio device names...";else if(!view.devices.error.empty())message+=" "+view.devices.error;
    }result=choice>=0 && size_t(choice)<message.size()?uint8_t(message[size_t(choice)]):0;return true;}
    if(id==2806 || id==2807){
        auto& catalog=Catalog();catalog.Refresh();const auto view=catalog.Snapshot();
        if(choice==0 || choice==1){std::lock_guard lock(preferences_mutex);auto& name=id==2806?input_device:output_device;const auto next=Name(CycleVoiceDevice(view,id==2806,name.data(),choice==0?-1:1));if(next!=name){name=next;ReleaseOnlineVoiceInput();SavePreferencesLocked();}}
        return true;
    }
    if(id==2808 || id==2809){
        static thread_local std::array<std::string,2> labels;const auto index=unsigned(id-2808);
        if(choice==0){auto& catalog=Catalog();catalog.Refresh();const auto view=catalog.Snapshot();std::lock_guard lock(preferences_mutex);labels[index]=VoiceDeviceLabel(view,index==0,(index==0?input_device:output_device).data());}
        result=choice>=0 && size_t(choice)<labels[index].size()?uint8_t(labels[index][size_t(choice)]):0;return true;
    }
    if(id==2810){if(choice==1)Catalog().Refresh(true);return true;}
    return false;
}
void PumpOnlineVoiceImpl(Room* room){
    struct Preserve {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~Preserve(){_mm_setcsr(fp);SetLastError(error);}}preserve;
    static std::unique_ptr<MediaWorker> worker;static Room* prior_room=nullptr;static Config prior;static uint64_t epoch=0,session=0;
    const bool active=room && room->state()==RoomState::Running && !REXCVAR_GET(em2_probe_online_voice);
    connected=active;
    if(!active)ReleaseOnlineVoiceInput();
    Config c;c.enabled=active && mode.load()!=0;c.capture=c.enabled && focused.load() && mode.load()>=2;c.talk=c.capture && Talking();c.volume=volume.load()*.25f;
    {std::lock_guard lock(preferences_mutex);c.input=input_device;c.output=output_device;}
    c.proximity=spatial_mode.load()==1;c.far_distance=ranges[range_index.load()];
    if(active){c.world=em2::CurrentNativeLoadGeneration();c.local_peer=room->local_peer();c.token=room->identity().session;}
    if(c.enabled)for(const auto& player:room->lobby().players)if(player.peer!=room->local_peer())c.roster|=uint16_t(1u<<(player.peer-1));
    if(prior_room!=room)++session;c.session=session;c.epoch=prior.epoch;
    if(prior_room!=room || c!=prior){c.epoch=++epoch;prior=c;prior_room=room;if(worker)worker->Configure(c);}
    if(c.enabled && !worker){worker=std::make_unique<MediaWorker>([] {return std::make_unique<Devices>();},[](Config& value){value.enabled&=connected.load() && mode.load()!=0;value.capture&=value.enabled && focused.load() && mode.load()>=2;value.talk&=value.capture && Talking();});worker->Configure(c);}
    if(c.enabled){
        if(!room->voice_ready())room->RequestVoice();
        const auto now=std::chrono::steady_clock::now();
        for(const auto& packet:room->TakeVoice())worker->Submit(c.epoch,packet.peer,packet.packet,now);
        VoicePacket packet;for(unsigned i=0;i<8 && worker->Take(c.epoch,packet);++i)if(Talking() && room->voice_ready())room->SendVoice(packet);
    }else if(room && !REXCVAR_GET(em2_probe_online_voice))room->TakeVoice();
    {std::lock_guard lock(status_mutex);status=!c.enabled?(mode.load()?"Voice is available during online gameplay.":"Voice is off."):worker->Status();if(status.empty())status=room->voice_ready()?(!focused.load()?"Microphone paused while the game is unfocused.":mode.load()==3?"Open microphone enabled.":c.capture?"Push to talk: hold the selected key or D-pad Down.":"Listening; microphone off."):"Connecting voice...";}
}
void PumpOnlineVoice(Room* room){
    try{PumpOnlineVoiceImpl(room);}catch(...){mode=0;connected=false;ReleaseOnlineVoiceInput();REXLOG_ERROR("Online voice disabled after a media bridge failure; gameplay retained");}
}
bool TestOnlineVoiceSettings(const std::filesystem::path& root){
    std::error_code ec;if(!std::filesystem::create_directory(root,ec) || ec)return false;
    struct Restore {
        VoicePreferences previous;std::filesystem::path file;std::string error;
        Restore(){std::lock_guard lock(preferences_mutex);previous={mode.load(),volume.load(),key_index.load(),spatial_mode.load(),range_index.load(),input_device.data(),output_device.data()};file=preferences_file;error=preferences_error;}
        ~Restore(){std::lock_guard lock(preferences_mutex);mode=previous.mode;volume=previous.volume;key_index=previous.key;spatial_mode=previous.positioning;range_index=previous.range;input_device=Name(previous.input);output_device=Name(previous.output);preferences_file=file;preferences_error=error;ReleaseOnlineVoiceInput();}
    }restore;
    bool ok=true;unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value){ok=false;REXLOG_ERROR("Native voice settings: {}",why);}};
    LoadOnlineVoiceSettings(root);check(mode==0 && volume==4,"fresh profile never enables microphone");
    const auto file=root/"voice-settings.ini";VoicePreferences p{2,5,1,0,2,"Saved microphone","Saved speakers"};std::string error;
    check(WriteVoicePreferences(file,p,error),"private profile fixture writes named devices");LoadOnlineVoiceSettings(root);
    check(mode==2 && input_device==Name(p.input) && output_device==Name(p.output),"runtime restores voice mode and exact saved device names without enumeration");
    int result=0;NativeOnlineVoiceOption(2800,3,result);NativeOnlineVoiceOption(2801,7,result);NativeOnlineVoiceOption(2802,2,result);NativeOnlineVoiceOption(2804,1,result);NativeOnlineVoiceOption(2805,4,result);
    p.mode=3;p.volume=7;p.key=2;p.positioning=1;p.range=4;check(ReadVoicePreferences(file)==p,"actual native option actions persist all choices and retain named devices");
    NativeOnlineVoiceOption(2800,0,result);p.mode=0;LoadOnlineVoiceSettings(root);check(mode==0 && ReadVoicePreferences(file)==p,"explicit Off remains Off after runtime reload");
    NativeOnlineVoiceOption(2800,999,result);check(result==0 && ReadVoicePreferences(file)==p,"invalid native selection cannot enable or persist microphone");
    {std::lock_guard lock(preferences_mutex);preferences_file=root/"missing"/"voice.ini";}
    NativeOnlineVoiceOption(2801,6,result);check(result==6 && !preferences_error.empty() && ReadVoicePreferences(file)==p,"save failure is reported without losing live choice or previous valid profile");
    std::filesystem::remove(file,ec);std::filesystem::remove(root,ec);
    REXLOG_INFO("Native voice settings {} {} checks: real option handlers, named-device load, persistence/reload, Off/invalid input and write-failure preservation; no device or microphone access",ok?"passed":"FAILED",checks);return ok;
}
bool TestOnlineVoiceRuntime(){
    struct FakeState {
        std::atomic<unsigned> made{0},destroyed{0},play{0},capture{0},closed{0},flush{0},received{0},encoded{0},pending{0};
        std::atomic<bool> block{false},fail_capture{false},throw_config{false},wrong_thread{false};
        DWORD owner=GetCurrentThreadId();
        std::atomic<uint64_t> scene_world{0};std::atomic<unsigned> scene_updates{0};
        std::atomic<unsigned> input_name{0},output_name{0};
    } state;
    struct Fake final:Audio {
        FakeState& s;bool recording=false;uint64_t sequence=0;
        explicit Fake(FakeState& state):s(state){++s.made;}
        ~Fake(){++s.destroyed;}
        void Thread(){if(GetCurrentThreadId()==s.owner)s.wrong_thread=true;}
        bool Playback(const char* name,std::string&)override{Thread();s.output_name=std::string_view(name)=="Speakers B"?2:std::string_view(name)=="Speakers A"?1:0;++s.play;const auto until=VoiceTime::clock::now()+std::chrono::seconds(2);while(s.block && VoiceTime::clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));return true;}
        bool Capture(const char* name,std::string& e)override{Thread();recording=false;s.input_name=std::string_view(name)=="Mic B"?2:std::string_view(name)=="Mic A"?1:0;++s.capture;if(s.fail_capture){e="injected device failure";return false;}recording=true;return true;}
        void CloseCapture()override{Thread();recording=false;++s.closed;}
        void FlushCapture()override{Thread();s.pending=0;++s.flush;}
        bool Poll(VoicePacket& p)override{Thread();if(!recording || !s.pending)return false;--s.pending;++s.encoded;p.sequence=sequence++;p.size=1;p.data[0]=1;return true;}
        void Submit(unsigned,const VoicePacket&,VoiceTime)override{Thread();++s.received;}
        void Configure(const Config&,const OnlineVoiceScene& scene)override{Thread();if(s.throw_config)throw std::runtime_error("injected media configuration failure");Scene(scene);}
        void Scene(const OnlineVoiceScene& scene)override{Thread();s.scene_world=scene.world;++s.scene_updates;}
    };
    bool ok=true;unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value){ok=false;REXLOG_ERROR("Voice media worker: {}",why);}return value;};
    auto wait=[&](auto done){const auto until=VoiceTime::clock::now()+std::chrono::seconds(3);do{if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(2));}while(VoiceTime::clock::now()<until);return false;};
    {
        std::atomic<bool> privacy{true};
        MediaWorker worker([&]{return std::make_unique<Fake>(state);},[&](Config& c){c.capture&=privacy.load();c.talk&=privacy.load();});
        std::this_thread::sleep_for(std::chrono::milliseconds(10));check(state.made==0,"default worker opens no devices without opt in");
        Config c;c.epoch=1;c.session=1;c.enabled=true;c.roster=2;
        state.block=true;worker.Configure(c);check(wait([&]{return state.play==1;}),"playback begins on worker");
        VoicePacket packet;packet.size=1;packet.data[0]=1;
        unsigned admitted=0;for(unsigned i=0;i<100;++i)admitted+=worker.Submit(c.epoch,2,packet,VoiceTime::clock::now());
        check(admitted==64 && !worker.Submit(c.epoch,1,packet,{}) && !worker.Submit(0,2,packet,{}),"bounded receive bridge rejects overflow, unadmitted speaker and stale epoch while device opening is blocked");
        state.block=false;check(wait([&]{return worker.Applied()==1 && state.received==64;}),"worker drains copied incoming audio after playback opens");
        check(state.capture==0,"listen-only never opens a microphone");
        c.epoch=2;c.capture=true;worker.Configure(c);check(wait([&]{return worker.Applied()==2;}),"explicit PTT configuration opens capture");
        state.pending=2;check(wait([&]{return state.encoded==2;}),"released PTT drains and discards capture");
        check(!worker.Take(c.epoch,packet),"released PTT produces no transmitted packet");
        c.epoch=3;c.talk=true;worker.Configure(c);check(wait([&]{return worker.Applied()==3 && state.flush==1;}),"PTT press flushes microphone backlog and codec history");
        state.pending=100;check(wait([&]{return state.pending==0;}),"capture processing remains bounded per worker iteration");
        unsigned transmitted=0;while(worker.Take(c.epoch,packet))++transmitted;
        check(transmitted==8,"transmit queue remains bounded under capture burst");
        privacy=false;check(wait([&]{return state.closed==1;}),"privacy revocation closes microphone even without another owner poll");
        check(!worker.Take(c.epoch,packet),"privacy revocation retires queued speech");
        privacy=true;check(wait([&]{return state.capture==2 && state.flush==2;}),"explicit renewed privacy policy opens fresh capture and flushes codec");
        state.pending=1;check(wait([&]{return state.pending==0;}),"captured frame queued before PTT release");
        c.epoch=4;c.talk=false;worker.Configure(c);check(!worker.Take(3,packet) && !worker.Take(4,packet),"PTT release immediately retires queued transmit epochs");
        c.epoch=5;++c.session;c.talk=true;worker.Configure(c);check(wait([&]{return worker.Applied()==5 && state.made==2 && state.destroyed==1;}),"new Room retires old decoder/devices and creates fresh media state");
        c.epoch=6;c.enabled=false;c.capture=false;c.talk=false;worker.Configure(c);check(wait([&]{return worker.Applied()==6 && state.destroyed==2;}),"Off releases devices on worker");
        state.fail_capture=true;c.epoch=7;c.enabled=true;c.capture=true;worker.Configure(c);check(wait([&]{return worker.Applied()==7;}),"failed microphone configuration completes without gameplay failure");
        check(worker.Status().find("injected device failure")!=std::string::npos,"actionable device failure status retained");
        const auto attempts=state.capture.load();std::this_thread::sleep_for(std::chrono::milliseconds(20));check(state.capture==attempts,"failed microphone does not retry in a busy loop");
        state.throw_config=true;c.epoch=8;++c.session;worker.Configure(c);check(wait([&]{return worker.Applied()==8;}),"audio exception contained at media boundary");
        check(worker.Status().find("injected media configuration failure")!=std::string::npos,"media exception retains bounded diagnostic");
    }
    check(state.made==state.destroyed && !state.wrong_thread,"all media objects released and no audio operation ran on caller");
    state.throw_config=false;state.fail_capture=false;
    {
        MediaWorker worker([&]{return std::make_unique<Fake>(state);});Config c;c.epoch=100;c.session=10;c.enabled=true;c.capture=true;c.input=Name("Mic A");c.output=Name("Speakers A");worker.Configure(c);
        check(wait([&]{return worker.Applied()==100 && state.input_name==1 && state.output_name==1;}),"saved named devices reach worker APIs");
        const auto made=state.made.load(),captures=state.capture.load();c.epoch=101;c.output=Name("Speakers B");worker.Configure(c);
        check(wait([&]{return worker.Applied()==101 && state.output_name==2;}) && state.made==made && state.capture==captures,"speaker switches live without reopening microphone, worker or Room");
        const auto playbacks=state.play.load();c.epoch=102;c.input=Name("Mic B");worker.Configure(c);
        check(wait([&]{return worker.Applied()==102 && state.input_name==2;}) && state.play==playbacks,"microphone switches live without reopening speaker");
        state.fail_capture=true;c.epoch=103;c.input=Name("Removed mic");worker.Configure(c);check(wait([&]{return worker.Applied()==103;}) && !worker.Status().empty(),"missing named microphone reports failure");
        state.fail_capture=false;c.epoch=104;c.input={};worker.Configure(c);check(wait([&]{return worker.Applied()==104 && state.input_name==0;}) && worker.Status().empty(),"System default recovers microphone live and clears stale error");
    }
    struct SceneRestore {SceneCopy old;SceneRestore(){std::lock_guard lock(scene_mutex);old=published_scene;}~SceneRestore(){std::lock_guard lock(scene_mutex);published_scene=old;}}restore_scene;
    OnlineVoiceScene scene;scene.world=9;scene.local_peer=3;scene.session[0]=37;scene.listener={true,9,{0,0,0}};scene.players[2]=scene.listener;
    scene.players[0]={true,9,{10,0,0}};scene.players[1]={true,9,{-20,0,0}};
    Config spatial;spatial.epoch=1;spatial.session=1;spatial.world=9;spatial.local_peer=3;spatial.token=scene.session;spatial.enabled=true;spatial.proximity=true;spatial.roster=3;
    check(PublishOnlineVoiceScene(scene),"finite native scene and camera basis accepted");
    VoiceTime captured;{std::lock_guard lock(scene_mutex);captured=published_scene.at;}
    check(SceneFor(spatial,captured+std::chrono::milliseconds(250)).scene.world==9 && !SceneFor(spatial,captured+std::chrono::milliseconds(251)).scene.world && !SceneFor(spatial,captured-std::chrono::nanoseconds(1)).scene.world,
        "scene freshness has exact bounded deadline and rejects future publication");
    auto wrong=spatial;wrong.token[0]^=1;check(!SceneFor(wrong,captured).scene.world,"old session cannot become a listener for new Room");
    wrong=spatial;++wrong.world;check(!SceneFor(wrong,captured).scene.world,"old level generation cannot become current audio positions");
    wrong=spatial;wrong.local_peer=1;check(!SceneFor(wrong,captured).scene.world,"different local player cannot supply listener or camera");
    auto invalid_scene=scene;invalid_scene.right={0,0,0};check(!PublishOnlineVoiceScene(invalid_scene),"degenerate camera publication clears previous valid scene");
    check(!SceneFor(spatial,VoiceTime::clock::now()).scene.world,"invalid replacement cannot retain old proximity gains");
    invalid_scene=scene;invalid_scene.players[0].point[0]=std::numeric_limits<float>::quiet_NaN();check(!PublishOnlineVoiceScene(invalid_scene),"nonfinite native actor cannot cross into mixer");
    invalid_scene=scene;invalid_scene.players[0].world=10;check(!PublishOnlineVoiceScene(invalid_scene),"mixed level generations rejected before publication");
    invalid_scene=scene;invalid_scene.listener.point[0]=1;check(!PublishOnlineVoiceScene(invalid_scene),"listener must be copied from local actor, not another camera position");
    state.throw_config=false;state.fail_capture=false;
    check(PublishOnlineVoiceScene(scene),"native scene republished after deliberate invalid cases");
    {
        MediaWorker worker([&]{return std::make_unique<Fake>(state);});worker.Configure(spatial);
        check(wait([&]{return worker.Applied()==1 && state.scene_world==9;}),"copied scene arrives on media worker");
        const auto devices=state.made.load(),updates=state.scene_updates.load();scene.players[0].point[0]=20;PublishOnlineVoiceScene(scene);
        check(wait([&]{return state.scene_updates>updates;} ) && state.made==devices && worker.Applied()==1,"position changes update mixer without recreating devices or changing transmit epoch");
        {std::lock_guard lock(scene_mutex);published_scene.at-=std::chrono::seconds(1);}
        check(wait([&]{return state.scene_world==0 && worker.Status().find("proximity voice muted")!=std::string::npos;}),"stale scene silences proximity even with no new native owner poll");
    }
    check(state.made==state.destroyed && !state.wrong_thread,"spatial worker releases devices and never invokes audio on publisher thread");
    const auto old_mode=mode.load(),old_volume=volume.load(),old_key=key_index.load(),old_spatial=spatial_mode.load(),old_range=range_index.load();const bool old_connected=connected.load(),old_focus=focused.load();int result=0;
    connected=true;focused=true;NativeOnlineVoiceOption(2800,2,result);
    check(OnlineVoiceKey(VK_F8,true) && Talking() && OnlineVoiceKey(VK_F8,false) && !Talking(),"native PTT mode gates keyboard hold/release");
    check(OnlineVoicePad(2)==2 && Talking(),"controller PTT consumes only D-pad Down");
    em2::SetChatControlCapture(true);check(OnlineVoicePad(2)==0 && !Talking(),"controller menu/chat navigation cannot trigger or be consumed as push to talk");em2::SetChatControlCapture(false);
    SetOnlineVoiceFocused(false);check(!Talking() && OnlineVoicePad(2)==0,"focus loss clears PTT and cannot restart from background controller sampling");
    NativeOnlineVoiceOption(2800,99,result);check(result==2,"invalid native option cannot enable a microphone");
    NativeOnlineVoiceOption(2804,1,result);check(result==1,"native options select actual proximity policy");NativeOnlineVoiceOption(2805,5,result);check(result==old_range,"invalid range cannot enter mixer settings");
    mode=old_mode;volume=old_volume;key_index=old_key;spatial_mode=old_spatial;range_index=old_range;connected=old_connected;focused=old_focus;ReleaseOnlineVoiceInput();
    REXLOG_INFO("Voice media worker {} {} checks: independent worker, bounded queues, opt-in/listen/PTT, stale capture fencing, session retirement, focus and device/exception isolation; fake devices only, no microphone hardware opened",ok?"passed":"FAILED",checks);return ok;
}
}
