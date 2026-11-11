#include "em1_audio_output.h"
#include <SDL.h>
#include <stdexcept>
using namespace em2::em1;
static void Need(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
extern "C" __declspec(dllexport) const char* run(const wchar_t* image){
  static std::string result;
  try {
    std::string error;auto rvz=RvzSource::Open(image,error);Need(bool(rvz),error);
    WiiAudioBank bank;Need(ReadOriginalWiiAudioBank(*rvz,"Global.bnk",bank,error),error);
    WiiEmbeddedDspSound source;Need(ResolveWiiEmbeddedDspSound(bank,0x09075f8e,source,error),error);
    NativeAudioPcm pcm;Need(DecodeWiiEmbeddedDspSound(bank,0x09075f8e,pcm,error),error);
    for(uint32_t rate:{16000u,48000u}){
      DspVoiceMixer mixer;DspVoiceMixer::Voice voice;
      Need(DspVoiceMixer::Prepare(source,1,rate,{.25f,.75f},1,voice,error),error);
      const auto id=mixer.Add(std::move(voice));Need(id!=0,"No voice ID");
      const auto scale=rate/16000;std::vector<float> samples((pcm.samples.size()*scale+5)*2);
      for(size_t at=0;at<samples.size();at+=26)mixer.Render(std::span(samples).subspan(at,std::min(size_t(26),samples.size()-at)));
      for(size_t i=0;i<pcm.samples.size()*scale;++i){
        const auto index=i/scale;const double phase=double(i%scale)/scale;
        const double expected=(pcm.samples[index]+(pcm.samples[std::min(index+1,pcm.samples.size()-1)]-pcm.samples[index])*phase)/32768.0;
        Need(std::abs(samples[i*2]-expected*.25)<.00001 && std::abs(samples[i*2+1]-expected*.75)<.00001,"Mix/resampling reference mismatch");
      }
      Need(mixer.Active()==0,"Completed voice still active");
      DspVoiceMixer::Voice next;Need(DspVoiceMixer::Prepare(source,1,rate,{1,1},1,next,error),error);
      auto new_id=mixer.Add(next);Need(new_id!=id && !mixer.Stop(id),"Stale handle affected replacement voice");
      for(size_t i=1;i<64;++i)Need(mixer.Add(next)!=0,"Early voice capacity limit");
      Need(!mixer.Add(next),"Voice capacity overrun");Need(mixer.Stop(new_id),"Stop failed");Need(mixer.Add(next)!=0,"Voice slot not reusable");
    }
    Need(SDL_GetNumAudioDrivers()>0,"No SDL audio driver compiled");
    {
      DspVoiceMixer mixer;DspVoiceMixer::Voice voice;
      Need(DspVoiceMixer::Prepare(source,1,48000,{1,1},1,voice,error),error);
      Need(BuildWiiVoiceAdmission(bank,0x09075f8e,7,50,voice.admission,error),error);
      Need(voice.admission.maximum==1 && voice.admission.kill_newest,"Original limit definition changed");
      const auto first=mixer.Add(voice);Need(first!=0,"First authored voice refused");
      Need(!mixer.Add(voice) && mixer.Active()==1,"Equal-priority newest voice not rejected");
      voice.admission.owner=9;const auto other=mixer.Add(voice);Need(other!=0 && mixer.Active()==2,"Independent owner blocked");
      voice.admission.owner=7;voice.admission.priority=75;const auto replacement=mixer.Add(voice);
      Need(replacement!=0 && mixer.Active()==2 && !mixer.Stop(first),"Priority steal failed or stale handle survived");
      Need(mixer.Stop(other) && mixer.Stop(replacement) && mixer.Active()==0,"Owner scope/cleanup failed");
      voice.admission.maximum=1;voice.admission.node=99;voice.admission.lineage_size=2;voice.admission.lineage={77,99};voice.admission.priority=40;
      Need(mixer.Add(voice)!=0,"Parent-group voice refused");
      voice.admission.lineage={88,99};voice.admission.priority=39;
      Need(!mixer.Add(voice),"Shared parent limit did not cover sibling lineage");
      voice.admission.priority=41;Need(mixer.Add(voice)!=0 && mixer.Active()==1,"Shared parent replacement failed");
    }
    SDL_setenv("SDL_AUDIODRIVER",SDL_GetAudioDriver(0),1);
    const auto before=SDL_WasInit(SDL_INIT_AUDIO);
    for(int pass=0;pass<2;++pass){
      DspAudioOutput output;Need(output.Open(error),error);Need(output.Open(error),error);
      const auto id=output.Play(source,1,{0,0},1,error);Need(id!=0,error);
      const auto start=SDL_GetTicks();while(output.Active() && SDL_GetTicks()-start<2500)SDL_Delay(5);
      Need(output.Active()==0 && output.CallbackCount()>0,"SDL callback did not consume voice");
      Need(output.Stop(id),"Completed voice ownership not released");Need(!output.Stop(id),"Stale output stop accepted");
    }
    Need(SDL_WasInit(SDL_INIT_AUDIO)==before,"SDL audio subsystem ownership leaked");
    result="PASS: original dry sound; PCM gain/resampling at16/48kHz;64 voices; authored owner/parent limits and stealing; stale IDs; SDL silent hardware callback consumption; repeated device shutdown";
  }catch(const std::exception& e){result=std::string("FAIL: ")+e.what();}
  return result.c_str();
}
