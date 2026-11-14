#include "online_voice_probe.h"
#include "online_native_menu.h"
#include "online_room.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/sha256.h>
#include <windows.h>
#include <immintrin.h>
#include <cmath>

REXCVAR_DEFINE_BOOL(em2_probe_online_voice,false,"Debug","Exact isolated native-menu Room only: generated Opus over encrypted UDP, no microphone or speaker device");
namespace em2::net {
void PumpPrivateOnlineVoice(Room* room){
    struct Probe {
        Room* owner=nullptr;VoiceEncoder encoder;VoiceMixer mixer;
        unsigned sent=0;std::array<unsigned,16> received{};bool decoded=false,announced=false;
        VoiceTime last{};
    };
    static thread_local std::unique_ptr<Probe> state;
    if(state && state->owner!=room)state.reset();
    if(!room || !REXCVAR_GET(em2_probe_online_voice) || PrivateNativeMenuRoom()!=room || room->state()!=RoomState::Running)return;
    struct Preserve{unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Preserve(){_mm_setcsr(csr);SetLastError(error);}}preserve;
    if(!state){state=std::make_unique<Probe>();state->owner=room;}
    auto& p=*state;const auto now=std::chrono::steady_clock::now();
    if(room->next_tick()>=80 && !room->voice_ready()){room->RequestVoice();return;}
    if(!room->voice_ready())return;
    if(!p.announced){p.announced=true;REXLOG_INFO("Native Room private voice ready: peer {}; separate encrypted UDP, generated audio only; no audio devices",room->local_peer());}
    if(room->next_tick()>=200 && p.sent<3 && (p.last==VoiceTime{} || now-p.last>=std::chrono::milliseconds(20))){
        VoicePcm pcm;for(unsigned i=0;i<pcm.size();++i){const double at=double(p.sent*kVoiceSamples+i)/kVoiceRate;pcm[i]=float(.15*std::sin(at*(220+room->local_peer()*55)*6.283185307179586));}
        VoicePacket packet;const auto sequence=100000+room->local_peer()*100+p.sent;
        if(p.encoder.Encode(sequence,pcm,packet) && room->SendVoice(packet)){
            const auto digest=rex::util::sha256(std::string_view(reinterpret_cast<const char*>(packet.data.data()),packet.size));
            REXLOG_INFO("Native Room private voice sent: peer {} sequence {} bytes {} digest {}; real Opus over UDP",room->local_peer(),packet.sequence,packet.size,digest);
            ++p.sent;p.last=now;
        }
    }
    for(const auto& m:room->TakeVoice()){
        const bool accepted=p.mixer.Submit(m.peer,m.packet,now);
        const auto digest=rex::util::sha256(std::string_view(reinterpret_cast<const char*>(m.packet.data.data()),m.packet.size));
        REXLOG_INFO("Native Room private voice received: peer {} sequence {} bytes {} digest {} decoder {}; host-authorized UDP sender",m.peer,m.packet.sequence,m.packet.size,digest,accepted);
        if(accepted && m.peer>=1 && m.peer<=16)++p.received[m.peer-1];
    }
    bool complete=p.sent==3;for(const auto& player:room->lobby().players)if(player.peer!=room->local_peer())complete&=p.received[player.peer-1]==3;
    if(complete && !p.decoded){
        std::array<float,kVoiceSamples*2> pcm;p.mixer.Render(now+std::chrono::milliseconds(60),pcm);double energy=0;bool finite=true;for(auto x:pcm){energy+=double(x)*x;finite&=std::isfinite(x) && std::abs(x)<=1;}
        p.decoded=true;REXLOG_INFO("Native Room private voice decoded: local {} speakers {} frames {} energy {:.6f} finite {}; generated PCM only, no device playback",room->local_peer(),room->lobby().players.size()-1,(room->lobby().players.size()-1)*3,energy,finite);
    }
}
}
