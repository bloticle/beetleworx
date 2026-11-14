#include "online_voice_audio.h"
#include <opus.h>
#include <SDL.h>
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <cstdlib>
#include <thread>

namespace em2::net {
namespace {
constexpr size_t QueueSize=8;
std::mutex audio_init_mutex;
bool Finite(std::span<const float> values){return std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);});}
bool ValidSettings(const VoiceMixSettings& s){
    if((s.mode!=VoiceMode::Party && s.mode!=VoiceMode::Proximity) || !Finite(s.listener.point) || !Finite(s.right) ||
       !std::isfinite(s.volume) || s.volume<0 || s.volume>2 || !std::isfinite(s.near_distance) ||
       !std::isfinite(s.far_distance) || s.near_distance<0 || s.far_distance<=s.near_distance)return false;
    const double n=double(s.right[0])*s.right[0]+double(s.right[1])*s.right[1]+double(s.right[2])*s.right[2];
    return n>.9 && n<1.1;
}
bool ValidPacket(const VoicePacket& p){
    return p.sequence<UINT64_MAX-QueueSize && p.size>0 && p.size<=p.data.size() &&
        opus_packet_get_nb_samples(p.data.data(),p.size,kVoiceRate)==kVoiceSamples;
}
struct Decoder {
    OpusDecoder* value=nullptr;
    Decoder(){int error=0;value=opus_decoder_create(kVoiceRate,1,&error);if(!value || error!=OPUS_OK)throw std::runtime_error("Cannot create voice decoder");}
    ~Decoder(){if(value)opus_decoder_destroy(value);}
    Decoder(const Decoder&)=delete;
};
struct Stream {
    Decoder decoder;
    struct Slot {bool present=false;VoicePacket packet;};
    std::array<Slot,QueueSize> slots{};
    size_t count=0;uint64_t next=0,floor=0;unsigned missing=0;
    bool started=false,have_floor=false,muted=false;float volume=1;
    VoicePosition position;VoiceTime first{};
    void Clear(){for(auto& s:slots)s.present=false;count=0;started=false;missing=0;opus_decoder_ctl(decoder.value,OPUS_RESET_STATE);}
    bool Submit(const VoicePacket& p,VoiceTime now){
        if(!ValidPacket(p) || (have_floor && p.sequence<floor))return false;
        if(muted)return true;
        if(!count && !started){next=p.sequence;first=now;}
        if(!started && p.sequence<next){if(next-p.sequence>=QueueSize)return false;next=p.sequence;}
        if(p.sequence<next)return false;
        if(p.sequence-next>=QueueSize){Clear();next=p.sequence;first=now;}
        auto& slot=slots[p.sequence%QueueSize];if(slot.present)return false;
        slot.present=true;slot.packet=p;++count;return true;
    }
    void Render(VoiceTime now,VoicePcm& output){
        output.fill(0);if(muted)return;
        if(!started){if(!count || (count<3 && now-first<std::chrono::milliseconds(60)))return;started=true;}
        if(next>=UINT64_MAX-QueueSize){Clear();return;}
        auto& slot=slots[next%QueueSize];int result=0;
        if(slot.present && slot.packet.sequence==next){
            result=opus_decode_float(decoder.value,slot.packet.data.data(),slot.packet.size,output.data(),kVoiceSamples,0);
            slot.present=false;--count;missing=0;
        }else{
            ++missing;
            // The existing jitter queue can already hold the next quantum.
            // Decode its redundant PREVIOUS frame, leaving that packet queued
            // for its own normal decode on the next render. Never wait for it
            // or borrow redundancy from a different timestamp/stream.
            const auto& following=slots[(next+1)%QueueSize];
            if(following.present && following.packet.sequence==next+1 &&
               opus_packet_has_lbrr(following.packet.data.data(),following.packet.size)==1){
                result=opus_decode_float(decoder.value,following.packet.data.data(),following.packet.size,output.data(),kVoiceSamples,1);
                if(result==kVoiceSamples)missing=0;
            }else if(missing<=3)result=opus_decode_float(decoder.value,nullptr,0,output.data(),kVoiceSamples,0);
        }
        floor=++next;have_floor=true;
        if(result!=kVoiceSamples || !Finite(output))output.fill(0);
        if(missing>3){
            started=false;missing=0;first=now;opus_decoder_ctl(decoder.value,OPUS_RESET_STATE);
            if(count){next=UINT64_MAX;for(const auto& pending:slots)if(pending.present)next=std::min(next,pending.packet.sequence);}
        }
    }
};
}
struct VoiceEncoder::State {
    OpusEncoder* value=nullptr;
    State(){
        int error=0;value=opus_encoder_create(kVoiceRate,1,OPUS_APPLICATION_VOIP,&error);
        if(!value || error!=OPUS_OK){if(value)opus_encoder_destroy(value);value=nullptr;throw std::runtime_error("Cannot create voice encoder");}
        if(opus_encoder_ctl(value,OPUS_SET_BITRATE(24000))!=OPUS_OK || opus_encoder_ctl(value,OPUS_SET_COMPLEXITY(5))!=OPUS_OK ||
           opus_encoder_ctl(value,OPUS_SET_INBAND_FEC(1))!=OPUS_OK || opus_encoder_ctl(value,OPUS_SET_PACKET_LOSS_PERC(10))!=OPUS_OK ||
           opus_encoder_ctl(value,OPUS_SET_DTX(1))!=OPUS_OK){opus_encoder_destroy(value);value=nullptr;throw std::runtime_error("Cannot configure voice codec");}
    }
    ~State(){if(value)opus_encoder_destroy(value);}
};
VoiceEncoder::VoiceEncoder():state_(std::make_unique<State>()){}
VoiceEncoder::~VoiceEncoder()=default;
void VoiceEncoder::Reset(){opus_encoder_ctl(state_->value,OPUS_RESET_STATE);}
bool VoiceEncoder::Encode(uint64_t sequence,std::span<const float> pcm,VoicePacket& out){
    if(pcm.size()!=kVoiceSamples || !Finite(pcm) || sequence>=UINT64_MAX-QueueSize)return false;
    VoicePcm limited;std::transform(pcm.begin(),pcm.end(),limited.begin(),[](float x){return std::clamp(x,-1.f,1.f);});
    VoicePacket candidate;candidate.sequence=sequence;
    const auto count=opus_encode_float(state_->value,limited.data(),kVoiceSamples,candidate.data.data(),int(candidate.data.size()));
    if(count<=0 || count>kVoicePacketBytes)return false;candidate.size=uint16_t(count);out=candidate;return true;
}
std::array<float,2> VoiceSpatialGain(const VoiceMixSettings& s,const VoicePosition& p){
    if(!ValidSettings(s))return {};
    double gain=s.volume,pan=0;
    if(s.mode==VoiceMode::Proximity){
        if(!s.listener.valid || !p.valid || !s.listener.world || p.world!=s.listener.world || !Finite(p.point))return {};
        std::array<double,3> d;double squared=0;
        for(unsigned i=0;i<3;++i){d[i]=double(p.point[i])-s.listener.point[i];squared+=d[i]*d[i];}
        const double distance=std::sqrt(squared);if(distance>=s.far_distance)return {};
        const double t=std::clamp((distance-s.near_distance)/(s.far_distance-s.near_distance),0.,1.);
        gain*=1-t*t*(3-2*t);
        if(distance>1.e-6){double right=0,norm=0;for(unsigned i=0;i<3;++i){right+=d[i]*s.right[i];norm+=double(s.right[i])*s.right[i];}pan=std::clamp(right/(distance*std::sqrt(norm)),-1.,1.);}
    }
    return {float(gain*std::sqrt((1-pan)*.5)),float(gain*std::sqrt((1+pan)*.5))};
}
struct VoiceMixer::State {
    std::array<Stream,16> peers;
    VoiceMixSettings settings;
    std::array<float,kVoiceSamples*2> mixed{};size_t cursor=kVoiceSamples;
    void Frame(VoiceTime now){
        mixed.fill(0);VoicePcm pcm;
        for(auto& peer:peers){peer.Render(now,pcm);const auto gain=VoiceSpatialGain(settings,peer.position);
            for(size_t i=0;i<pcm.size();++i){mixed[i*2]+=pcm[i]*gain[0]*peer.volume;mixed[i*2+1]+=pcm[i]*gain[1]*peer.volume;}}
        for(auto& value:mixed)value=std::clamp(value,-1.f,1.f);
    }
};
VoiceMixer::VoiceMixer():state_(std::make_unique<State>()){}
VoiceMixer::~VoiceMixer()=default;
bool VoiceMixer::Submit(unsigned peer,const VoicePacket& packet,VoiceTime now){return peer>=1 && peer<=16 && state_->peers[peer-1].Submit(packet,now);}
bool VoiceMixer::Configure(const VoiceMixSettings& s){if(!ValidSettings(s))return false;state_->settings=s;return true;}
bool VoiceMixer::ConfigureScene(const VoiceMixSettings& settings,const std::array<VoicePeerMix,16>& peers){
    // Validate the whole publication before changing any active stream.
    if(!ValidSettings(settings))return false;
    for(const auto& peer:peers)if(!std::isfinite(peer.volume) || peer.volume<0 || peer.volume>2 || !Finite(peer.position.point))return false;
    state_->settings=settings;
    for(unsigned i=0;i<peers.size();++i)SetPeer(i+1,peers[i].muted,peers[i].volume,peers[i].position);
    return true;
}
bool VoiceMixer::SetPeer(unsigned peer,bool muted,float volume,const VoicePosition& p){
    if(!peer || peer>16 || !std::isfinite(volume) || volume<0 || volume>2 || !Finite(p.point))return false;
    auto& stream=state_->peers[peer-1];if(stream.muted!=muted){stream.Clear();state_->mixed.fill(0);}
    stream.muted=muted;stream.volume=volume;stream.position=p;return true;
}
void VoiceMixer::ResetPeer(unsigned peer){if(peer && peer<=16){auto& stream=state_->peers[peer-1];stream.Clear();stream.have_floor=false;stream.floor=stream.next=0;state_->mixed.fill(0);}}
size_t VoiceMixer::Pending(unsigned peer)const{return peer && peer<=16?state_->peers[peer-1].count:0;}
void VoiceMixer::Render(VoiceTime now,std::span<float> stereo){
    std::fill(stereo.begin(),stereo.end(),0);if(stereo.size()%2)return;
    size_t written=0;
    while(written<stereo.size()/2){
        if(state_->cursor==kVoiceSamples){state_->Frame(now+std::chrono::microseconds(written*1000000/kVoiceRate));state_->cursor=0;}
        const auto count=std::min(size_t(kVoiceSamples)-state_->cursor,stereo.size()/2-written);
        std::copy_n(state_->mixed.data()+state_->cursor*2,count*2,stereo.data()+written*2);state_->cursor+=count;written+=count;
    }
}
struct VoiceAudio::State {
    std::mutex api;
    SDL_AudioDeviceID capture=0,playback=0;bool initialized=false;
    VoiceEncoder encoder;VoiceMixer mixer;
    struct Captured {uint64_t sequence=0;VoicePcm pcm;};
    std::array<Captured,QueueSize> frames;
    std::atomic<uint64_t> read{0},write{0},dropped{0};
    VoicePcm partial;size_t used=0;uint64_t sequence=0;
    bool Initialize(std::string& error){if(!initialized){if(SDL_InitSubSystem(SDL_INIT_AUDIO)){error=SDL_GetError();return false;}initialized=true;}return true;}
    static void Capture(void* data,Uint8* bytes,int length){
        auto& s=*static_cast<State*>(data);if(length<=0 || length%sizeof(float))return;
        auto* pcm=reinterpret_cast<const float*>(bytes);size_t left=size_t(length)/sizeof(float);
        while(left){const auto take=std::min(left,size_t(kVoiceSamples)-s.used);std::copy_n(pcm,take,s.partial.data()+s.used);s.used+=take;pcm+=take;left-=take;
            if(s.used==kVoiceSamples){const auto w=s.write.load(std::memory_order_relaxed);
                if(w-s.read.load(std::memory_order_acquire)<QueueSize){auto& f=s.frames[w%QueueSize];f.sequence=s.sequence;f.pcm=s.partial;s.write.store(w+1,std::memory_order_release);}else ++s.dropped;
                ++s.sequence;s.used=0;}}
    }
    static void Playback(void* data,Uint8* bytes,int length){
        if(length<=0)return;SDL_memset(bytes,0,length);if(length%(2*sizeof(float)))return;
        static_cast<State*>(data)->mixer.Render(std::chrono::steady_clock::now(),std::span(reinterpret_cast<float*>(bytes),size_t(length)/sizeof(float)));
    }
    bool Pop(VoicePacket& out){
        const auto r=read.load(std::memory_order_relaxed);if(r==write.load(std::memory_order_acquire))return false;
        const auto frame=frames[r%QueueSize];read.store(r+1,std::memory_order_release);return encoder.Encode(frame.sequence,frame.pcm,out);
    }
    ~State(){std::lock_guard lock(audio_init_mutex);if(capture)SDL_CloseAudioDevice(capture);if(playback)SDL_CloseAudioDevice(playback);if(initialized)SDL_QuitSubSystem(SDL_INIT_AUDIO);}
};
VoiceDevices EnumerateVoiceDevices(){
    std::lock_guard lock(audio_init_mutex);VoiceDevices result;
    if(SDL_InitSubSystem(SDL_INIT_AUDIO)){result.error=std::string(SDL_GetError()).substr(0,160);return result;}
    struct Quit {~Quit(){SDL_QuitSubSystem(SDL_INIT_AUDIO);}}quit;
    for(unsigned capture=0;capture<2;++capture){const auto count=SDL_GetNumAudioDevices(int(capture));if(count<0){result.error="Device names unavailable; System default is still available.";continue;}
        auto& names=capture?result.input:result.output;
        for(int i=0;i<std::min(count,32);++i)if(const auto* name=SDL_GetAudioDeviceName(i,int(capture))){size_t length=0;while(length<=512 && name[length])++length;
            if(!length || length>512)continue;std::string value(name,length);
            if(std::any_of(value.begin(),value.end(),[](unsigned char c){return c<32 || c==127;}) || std::find(names.begin(),names.end(),value)!=names.end())continue;names.push_back(std::move(value));}
    }
    return result;
}
VoiceAudio::VoiceAudio():state_(std::make_unique<State>()){}
VoiceAudio::~VoiceAudio()=default;
bool VoiceAudio::OpenCapture(std::string name,std::string& error){
    auto& s=*state_;std::lock_guard lock(s.api);std::lock_guard lifecycle(audio_init_mutex);if(s.capture){SDL_CloseAudioDevice(s.capture);s.capture=0;}
    if(!s.Initialize(error))return false;
    s.read.store(0);s.write.store(0);s.dropped.store(0);s.used=0;
    s.sequence=std::max(s.sequence,uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()/20));
    SDL_AudioSpec spec{};spec.freq=kVoiceRate;spec.format=AUDIO_F32SYS;spec.channels=1;spec.samples=512;spec.callback=&State::Capture;spec.userdata=&s;
    s.capture=SDL_OpenAudioDevice(name.empty()?nullptr:name.c_str(),1,&spec,nullptr,0);
    if(!s.capture){error=SDL_GetError();return false;}s.encoder.Reset();SDL_PauseAudioDevice(s.capture,0);error.clear();return true;
}
bool VoiceAudio::OpenPlayback(std::string name,std::string& error){
    auto& s=*state_;std::lock_guard lock(s.api);std::lock_guard lifecycle(audio_init_mutex);if(s.playback){SDL_CloseAudioDevice(s.playback);s.playback=0;}
    if(!s.Initialize(error))return false;
    for(unsigned peer=1;peer<=16;++peer)s.mixer.ResetPeer(peer);
    SDL_AudioSpec spec{};spec.freq=kVoiceRate;spec.format=AUDIO_F32SYS;spec.channels=2;spec.samples=512;spec.callback=&State::Playback;spec.userdata=&s;
    s.playback=SDL_OpenAudioDevice(name.empty()?nullptr:name.c_str(),0,&spec,nullptr,0);
    if(!s.playback){error=SDL_GetError();return false;}SDL_PauseAudioDevice(s.playback,0);error.clear();return true;
}
void VoiceAudio::CloseCapture(){auto& s=*state_;std::lock_guard lock(s.api);std::lock_guard lifecycle(audio_init_mutex);if(s.capture){SDL_CloseAudioDevice(s.capture);s.capture=0;}s.read.store(0);s.write.store(0);}
void VoiceAudio::FlushCapture(){
    auto& s=*state_;std::lock_guard lock(s.api);if(s.capture)SDL_LockAudioDevice(s.capture);
    s.read.store(0);s.write.store(0);s.used=0;++s.sequence;s.encoder.Reset();
    if(s.capture)SDL_UnlockAudioDevice(s.capture);
}
void VoiceAudio::ClosePlayback(){auto& s=*state_;std::lock_guard lock(s.api);std::lock_guard lifecycle(audio_init_mutex);if(s.playback){SDL_CloseAudioDevice(s.playback);s.playback=0;}for(unsigned peer=1;peer<=16;++peer)s.mixer.ResetPeer(peer);}
bool VoiceAudio::PollCapture(VoicePacket& out){
    auto& s=*state_;std::lock_guard lock(s.api);return s.capture && s.Pop(out);
}
bool VoiceAudio::Submit(unsigned peer,const VoicePacket& p,VoiceTime now){auto& s=*state_;std::lock_guard lock(s.api);if(!s.playback)return false;SDL_LockAudioDevice(s.playback);const bool ok=s.mixer.Submit(peer,p,now);SDL_UnlockAudioDevice(s.playback);return ok;}
bool VoiceAudio::Configure(const VoiceMixSettings& settings){auto& s=*state_;std::lock_guard lock(s.api);if(s.playback)SDL_LockAudioDevice(s.playback);const bool ok=s.mixer.Configure(settings);if(s.playback)SDL_UnlockAudioDevice(s.playback);return ok;}
bool VoiceAudio::ConfigureScene(const VoiceMixSettings& settings,const std::array<VoicePeerMix,16>& peers){auto& s=*state_;std::lock_guard lock(s.api);if(s.playback)SDL_LockAudioDevice(s.playback);const bool ok=s.mixer.ConfigureScene(settings,peers);if(s.playback)SDL_UnlockAudioDevice(s.playback);return ok;}
bool VoiceAudio::SetPeer(unsigned peer,bool muted,float gain,const VoicePosition& p){auto& s=*state_;std::lock_guard lock(s.api);if(s.playback)SDL_LockAudioDevice(s.playback);const bool ok=s.mixer.SetPeer(peer,muted,gain,p);if(s.playback)SDL_UnlockAudioDevice(s.playback);return ok;}
uint64_t VoiceAudio::DroppedCaptureFrames()const{return state_->dropped.load();}
bool TestOnlineVoiceAudio(){
    bool ok=true;unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value){ok=false;REXLOG_ERROR("Voice audio regression: {}",why);}};
    const auto t=VoiceTime{}+std::chrono::seconds(100);
    auto tone=[](unsigned frame){VoicePcm pcm;for(unsigned i=0;i<kVoiceSamples;++i){const double at=double(frame*kVoiceSamples+i)/kVoiceRate;pcm[i]=float(.2*std::sin(at*440*6.283185307179586)+.05*std::sin(at*880*6.283185307179586));}return pcm;};
    VoiceEncoder encoder;std::array<VoicePacket,32> packets;size_t bytes=0;
    for(unsigned i=0;i<packets.size();++i){check(encoder.Encode(i,tone(i),packets[i]) && ValidPacket(packets[i]),"real Opus frame encoding");bytes+=packets[i].size;}
    check(bytes<packets.size()*200,"speech-size compressed frames, not raw PCM");
    // Decode a real missing packet from actual encoder redundancy. Compare
    // every output quantum against an independent Opus decoder, including
    // decoding the lookahead normally after using its previous-frame FEC.
    unsigned recovered=0;
    for(unsigned i=9;i+2<packets.size();++i)if(opus_packet_has_lbrr(packets[i].data.data(),packets[i].size)==1){recovered=i-1;break;}
    check(recovered!=0,"production encoder emits actual in-band redundant audio");
    if(recovered)for(unsigned scenario=0;scenario<4;++scenario){
        Stream stream;Decoder reference,concealed;VoicePcm actual,expected,plc;
        auto samples=packets;for(auto& packet:samples)packet.sequence+=100+scenario*33;
        std::array<bool,32> lost{};lost[recovered]=true;
        if(scenario==1)lost[recovered-1]=true;
        if(scenario==2)for(unsigned i=1;i<=3;++i)lost[recovered-i]=true;
        if(scenario==3)lost[recovered+1]=true;
        for(unsigned i=0;i<3;++i)check(stream.Submit(samples[i],t),"FEC initial jitter buffering");
        double fec_difference=0;
        for(unsigned i=0;i<packets.size();++i){
            if(i && i+2<packets.size() && !lost[i+2])check(stream.Submit(samples[i+2],t),"FEC future packet admission");
            stream.Render(t+std::chrono::milliseconds(20*i),actual);
            const bool fec=lost[i] && i+1<packets.size() && !lost[i+1] && opus_packet_has_lbrr(packets[i+1].data.data(),packets[i+1].size)==1;
            const auto& packet=packets[fec?i+1:i];
            const auto n=opus_decode_float(reference.value,lost[i] && !fec?nullptr:packet.data.data(),lost[i] && !fec?0:packet.size,expected.data(),kVoiceSamples,fec);
            const auto p=opus_decode_float(concealed.value,lost[i]?nullptr:packets[i].data.data(),lost[i]?0:packets[i].size,plc.data(),kVoiceSamples,0);
            check(n==kVoiceSamples && p==kVoiceSamples && actual==expected,"jitter FEC matches independent sequential decoder, without consuming lookahead twice");
            if(fec)for(unsigned j=0;j<kVoiceSamples;++j)fec_difference+=std::abs(double(actual[j])-plc[j]);
            if(lost[i])check(!stream.Submit(samples[i],t),"late original cannot replay a recovered quantum");
        }
        check(fec_difference>.001,"actual redundant audio differs from ordinary loss concealment");
        check(stream.count==0,"FEC retains then consumes each future payload exactly once");
    }
    encoder.Reset();VoicePacket reset_packet;check(encoder.Encode(1000,tone(0),reset_packet) && reset_packet.size==packets[0].size && std::equal(reset_packet.data.begin(),reset_packet.data.begin()+reset_packet.size,packets[0].data.begin()),"reopening capture resets codec history without restarting source sequence");
    VoicePacket unchanged=packets[0],bad=unchanged;auto invalid=tone(0);invalid[0]=std::numeric_limits<float>::quiet_NaN();
    check(!encoder.Encode(40,invalid,bad) && bad.data==unchanged.data && bad.size==unchanged.size,"nonfinite capture rejected without output mutation");
    check(!encoder.Encode(40,std::span(invalid).first(100),bad) && !encoder.Encode(UINT64_MAX,tone(0),bad),"invalid quantum/sequence rejected");
    VoiceMixer mixer;VoiceMixSettings settings;check(mixer.Configure(settings),"party settings accepted");
    check(mixer.Submit(1,packets[1],t) && mixer.Submit(1,packets[0],t) && mixer.Submit(1,packets[2],t),"reordered initial datagrams accepted");
    check(!mixer.Submit(1,packets[1],t),"duplicate rejected");
    std::array<float,kVoiceSamples*2> output;double energy=0;
    for(unsigned frame=0;frame<24;++frame){
        if(frame>=3)check(mixer.Submit(1,packets[frame],t+std::chrono::milliseconds(frame*20)),"continuous sequenced input");
        mixer.Render(t+std::chrono::milliseconds(frame*20),output);
        check(Finite(output),"decoded output remains finite");
        for(size_t i=0;i<kVoiceSamples;++i){check(std::abs(output[2*i]-output[2*i+1])<1.e-6f,"party voice centered in stereo");if(frame>=4){energy+=double(output[2*i])*output[2*i];}}
    }
    check(energy>20 && energy<2000,"actual speech-band waveform survives lossy codec and mixer");
    // Independently measure a recovered steady frame's fundamental: compare its
    // Fourier energy at the input's 440 Hz with an unrelated 3 kHz tone.
    double real=0,imag=0,off_real=0,off_imag=0;
    for(unsigned i=0;i<kVoiceSamples;++i){const auto value=output[i*2];const double at=double(i)/kVoiceRate;real+=value*std::cos(at*440*6.283185307179586);imag+=value*std::sin(at*440*6.283185307179586);off_real+=value*std::cos(at*3000*6.283185307179586);off_imag+=value*std::sin(at*3000*6.283185307179586);}
    check(real*real+imag*imag>100*(off_real*off_real+off_imag*off_imag+1.e-8),"decoded signal preserves transmitted fundamental");
    check(!mixer.Submit(0,packets[24],t) && !mixer.Submit(17,packets[24],t) && !mixer.Submit(1,packets[0],t),"invalid peer and replay rejected");
    bad=packets[24];bad.size=401;check(!mixer.Submit(1,bad,t),"oversized voice frame rejected");bad.size=0;check(!mixer.Submit(1,bad,t),"empty frame rejected");
    bad=packets[24];bad.size=1;bad.data[0]=0;check(!mixer.Submit(1,bad,t),"wrong-duration Opus packet rejected");
    for(unsigned i=0;i<20;++i)mixer.Render(t+std::chrono::seconds(1)+std::chrono::milliseconds(i*20),output);
    check(std::all_of(output.begin(),output.end(),[](float x){return x==0;}),"loss concealment ends in silence, never loops stale speech");
    for(unsigned i=0;i<32;++i){auto packet=packets[i];packet.sequence=100+i;check(mixer.Submit(1,packet,t+std::chrono::seconds(2)),"backlog recency admission");check(mixer.Pending(1)<=QueueSize,"bounded backlog under burst");}
    check(mixer.SetPeer(1,true,1,{}) && mixer.Pending(1)==0,"muting discards queued audio");
    mixer.Render(t+std::chrono::seconds(2),output);check(std::all_of(output.begin(),output.end(),[](float x){return x==0;}),"muted output silent");
    check(mixer.SetPeer(1,false,1,{}) && !mixer.SetPeer(1,false,-1,{}) && !mixer.SetPeer(17,false,1,{}),"peer volume/identity validation");
    settings.mode=VoiceMode::Proximity;settings.listener={true,7,{0,0,0}};settings.near_distance=2;settings.far_distance=30;
    const auto close=VoiceSpatialGain(settings,{true,7,{0,0,1}}),distant=VoiceSpatialGain(settings,{true,7,{0,0,20}}),right=VoiceSpatialGain(settings,{true,7,{1,0,0}}),left=VoiceSpatialGain(settings,{true,7,{-1,0,0}});
    check(close[0]>distant[0] && close[0]==close[1] && distant[0]>0,"distance attenuation with centered front source");
    check(right[1]>right[0] && left[0]>left[1],"listener-relative stereo direction");
    check(VoiceSpatialGain(settings,{true,7,{0,0,30}})==std::array<float,2>{} && VoiceSpatialGain(settings,{true,8,{0,0,1}})==std::array<float,2>{} && VoiceSpatialGain(settings,{})==std::array<float,2>{},"out-of-range/missing/other-world voice inaudible");
    {
        VoiceMixer atomic,reference;std::array<VoicePeerMix,16> peers;
        for(auto& peer:peers)peer.muted=true;peers[0]={false,1,{true,7,{10,0,0}}};
        check(atomic.ConfigureScene(settings,peers) && reference.ConfigureScene(settings,peers),"complete proximity publication accepted atomically");
        for(unsigned i=0;i<3;++i)check(atomic.Submit(1,packets[i],t) && reference.Submit(1,packets[i],t),"atomic publication waveform admission");
        std::array<float,kVoiceSamples*2> expected;
        atomic.Render(t,output);reference.Render(t,expected);check(output==expected,"atomic publication uses actual spatial decoder output");
        auto invalid_peers=peers;invalid_peers[0].muted=true;invalid_peers[15].position.point[0]=std::numeric_limits<float>::quiet_NaN();
        check(!atomic.ConfigureScene(settings,invalid_peers) && atomic.Pending(1)==2,"invalid final peer cannot partially mute or flush earlier valid streams");
        atomic.Render(t+std::chrono::milliseconds(20),output);reference.Render(t+std::chrono::milliseconds(20),expected);
        check(output==expected,"rejected publication preserves complete active mixer state");
        double left_energy=0,right_energy=0;for(unsigned i=0;i<kVoiceSamples;++i){left_energy+=double(output[2*i])*output[2*i];right_energy+=double(output[2*i+1])*output[2*i+1];}
        check(right_energy>left_energy+1.e-5,"native right basis drives actual right-channel audio energy");
        peers[0].muted=true;check(atomic.ConfigureScene(settings,peers) && atomic.Pending(1)==0,"missing/stale source mutes and discards queued speech in one publication");
        atomic.Render(t+std::chrono::milliseconds(40),output);check(std::all_of(output.begin(),output.end(),[](float value){return value==0;}),"invalidated scene produces silence instead of stale proximity audio");
    }
    auto invalid_settings=settings;invalid_settings.right={0,0,0};check(!mixer.Configure(invalid_settings),"invalid camera right vector rejected");
    invalid_settings=settings;invalid_settings.volume=std::numeric_limits<float>::infinity();check(!mixer.Configure(invalid_settings),"nonfinite settings rejected");
    settings.mode=VoiceMode::Party;check(VoiceSpatialGain(settings,{})[0]>0,"party/lobby voice does not require actor positions");
    VoiceMixer sixteen;std::array<float,1920> mixed;double maximum_us=0,total_us=0;
    for(unsigned frame=0;frame<32;++frame){
        for(unsigned peer=1;peer<=16;++peer)check(sixteen.Submit(peer,packets[frame],t+std::chrono::milliseconds(frame*20)),"sixteen independent decoder queues");
        const auto begin=std::chrono::steady_clock::now();sixteen.Render(t+std::chrono::milliseconds(frame*20+60),mixed);const auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count();maximum_us=std::max(maximum_us,us);total_us+=us;
        check(Finite(mixed) && std::all_of(mixed.begin(),mixed.end(),[](float x){return std::abs(x)<=1;}),"sixteen-source mix bounded without NaNs/clipping overflow");
    }
    {
        // Actual callback bodies, fed generated fragmented PCM without opening
        // an OS device. This covers the ring/encoder path even on SDK builds
        // without SDL's dummy backend; it does not establish hardware support.
        const auto audio_before=SDL_WasInit(SDL_INIT_AUDIO);VoiceAudio callbacks;auto& state=*callbacks.state_;
        check(SDL_WasInit(SDL_INIT_AUDIO)==audio_before,"construction never initializes audio devices");
        state.sequence=1000;
        for(unsigned frame=0;frame<24;++frame){auto pcm=tone(frame);for(size_t at=0;at<pcm.size();){const auto n=std::min(size_t(317),pcm.size()-at);VoiceAudio::State::Capture(&state,reinterpret_cast<Uint8*>(pcm.data()+at),int(n*sizeof(float)));at+=n;}}
        check(callbacks.DroppedCaptureFrames()==16 && state.write.load()==8 && state.used==0,"fragmented callback quanta and exact bounded overflow");
        VoicePacket captured;unsigned count=0;while(state.Pop(captured)){check(ValidPacket(captured) && captured.sequence==1000+count,"callback PCM to real Opus worker with source timestamp");++count;}
        check(count==8 && !callbacks.PollCapture(captured),"no public capture while device is disabled");
        auto stale=tone(7);VoiceAudio::State::Capture(&state,reinterpret_cast<Uint8*>(stale.data()),int(sizeof(stale)));
        VoiceAudio::State::Capture(&state,reinterpret_cast<Uint8*>(stale.data()),317*sizeof(float));
        const auto before_flush=state.sequence;callbacks.FlushCapture();
        check(state.used==0 && state.read==state.write && state.sequence>before_flush && !state.Pop(captured),"PTT flush drops complete and partial callback PCM while preserving monotonic timestamp");
        auto fresh=tone(0);VoiceAudio::State::Capture(&state,reinterpret_cast<Uint8*>(fresh.data()),int(sizeof(fresh)));
        check(state.Pop(captured) && captured.size==packets[0].size && std::equal(captured.data.begin(),captured.data.begin()+captured.size,packets[0].data.begin()),"first post-PTT Opus packet has fresh encoder history, without prior PCM or redundant audio");
        for(unsigned i=0;i<3;++i)check(state.mixer.Submit(1,packets[i],std::chrono::steady_clock::now()),"callback output source admission");
        std::array<float,1024> callback_output;VoiceAudio::State::Playback(&state,reinterpret_cast<Uint8*>(callback_output.data()),int(sizeof(callback_output)));
        check(Finite(callback_output),"actual playback callback fills arbitrary hardware quantum");
        callbacks.CloseCapture();check(!state.Pop(captured),"closing capture flushes pending PCM");
    }
    {
        VoiceAudio callbacks;auto& state=*callbacks.state_;state.sequence=2000;std::atomic<bool> done{false};
        std::jthread producer([&]{for(unsigned frame=0;frame<500;++frame){auto pcm=tone(frame);VoiceAudio::State::Capture(&state,reinterpret_cast<Uint8*>(pcm.data()),int(sizeof(pcm)));}done.store(true,std::memory_order_release);});
        unsigned received=0;uint64_t previous=0;VoicePacket packet;
        while(!done.load(std::memory_order_acquire) || state.read.load()!=state.write.load()){
            if(state.Pop(packet)){check(ValidPacket(packet) && packet.sequence>=2000 && packet.sequence<2500 && (!received || packet.sequence>previous),"concurrent callback/encoder ownership and source order");previous=packet.sequence;++received;}
            else std::this_thread::yield();
        }
        producer.join();check(received>0 && received+callbacks.DroppedCaptureFrames()==500 && state.used==0,"concurrent capture accounting includes every encoded or dropped frame");
    }
    // Exercise actual SDL callbacks only with the explicit dummy backend. A
    // normal regression invocation must never open the user's real microphone.
    bool dummy=false,has_dummy=false;for(int i=0;i<SDL_GetNumAudioDrivers();++i)if(std::strcmp(SDL_GetAudioDriver(i),"dummy")==0)has_dummy=true;
    bool requested_dummy=false;char* driver=nullptr;size_t driver_size=0;
    if(_dupenv_s(&driver,&driver_size,"SDL_AUDIODRIVER")==0 && driver)requested_dummy=std::strcmp(driver,"dummy")==0;std::free(driver);
    if(requested_dummy && !has_dummy)REXLOG_WARN("Voice OS-device test unavailable: SDK SDL was built without the dummy backend; callback bodies tested with generated PCM; no real device opened");
    if(requested_dummy && has_dummy){
        VoiceAudio devices;VoicePacket captured;std::string error;
        check(!devices.PollCapture(captured) && devices.DroppedCaptureFrames()==0,"construction leaves microphone disabled");
        const bool playback=devices.OpenPlayback("",error);check(playback,"SDL dummy playback opens");
        if(!playback){std::string backends;for(int i=0;i<SDL_GetNumAudioDrivers();++i){if(i)backends+=", ";backends+=SDL_GetAudioDriver(i);}REXLOG_ERROR("Isolated voice device open failed: {}; compiled audio backends: {}",error,backends);}
        const char* actual=SDL_GetCurrentAudioDriver();dummy=actual && std::strcmp(actual,"dummy")==0;
        check(dummy,"device test is isolated from hardware");
        if(dummy){
            check(devices.OpenCapture("",error),"SDL dummy capture opens");
            for(unsigned i=0;i<3;++i)check(devices.Submit(1,packets[i],std::chrono::steady_clock::now()),"real SDL output accepts codec packets");
            SDL_Delay(450);check(devices.DroppedCaptureFrames()>0,"slow worker drops capture frames instead of growing latency");
            unsigned count=0;while(count<16 && devices.PollCapture(captured)){check(ValidPacket(captured),"real SDL capture callback accumulates exact codec quanta");++count;}
            check(count>0 && count<=QueueSize+1,"bounded audio callback/worker transfer");devices.CloseCapture();check(!devices.PollCapture(captured),"disabled microphone cannot return queued audio");
        }
        devices.ClosePlayback();
    }
    REXLOG_INFO("Voice audio {} {} checks: real static Opus, waveform/reorder/replay/loss/backlog/mute, proximity/world/pan, sixteen decoders; SDL dummy devices {}; 16-source 20ms mix average {:.3f} ms max {:.3f} ms. No network, microphone hardware, or playable voice assertion",ok?"passed":"FAILED",checks,dummy,total_us/32000,maximum_us/1000);
    return ok;
}
}
