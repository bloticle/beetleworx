#include "em1_audio_output.h"
#include <SDL.h>
#include <atomic>
namespace em2::em1 {
struct DspAudioOutput::State {
  SDL_AudioDeviceID device=0;
  bool initialized=false;
  DspVoiceMixer mixer;
  std::atomic<uint64_t> callbacks{0};
  static void Callback(void* data,Uint8* bytes,int length){
    auto& state=*static_cast<State*>(data);
    SDL_memset(bytes,0,length);
    if(length>0 && length%8==0)state.mixer.Render(std::span(reinterpret_cast<float*>(bytes),size_t(length)/sizeof(float)));
    ++state.callbacks;
  }
  ~State(){if(device)SDL_CloseAudioDevice(device);if(initialized)SDL_QuitSubSystem(SDL_INIT_AUDIO);}
};
DspAudioOutput::DspAudioOutput()=default;
DspAudioOutput::~DspAudioOutput()=default;
bool DspAudioOutput::Open(std::string& error){
  if(state_){error.clear();return true;}
  auto candidate=std::make_unique<State>();
  if(SDL_InitSubSystem(SDL_INIT_AUDIO)){error=SDL_GetError();return false;}candidate->initialized=true;
  SDL_AudioSpec spec{};spec.freq=48000;spec.format=AUDIO_F32SYS;spec.channels=2;spec.samples=512;
  spec.callback=&State::Callback;spec.userdata=candidate.get();
  candidate->device=SDL_OpenAudioDevice(nullptr,0,&spec,nullptr,0);
  if(!candidate->device){error=SDL_GetError();return false;}
  state_=std::move(candidate);SDL_PauseAudioDevice(state_->device,0);error.clear();return true;
}
uint64_t DspAudioOutput::Play(const WiiEmbeddedDspSound& source,uint16_t passes,std::array<float,2> gain,double pitch,std::string& error,const WiiVoiceAdmission& admission){
  if(!state_){error="Native DSP audio output is closed";return 0;}
  DspVoiceMixer::Voice voice;
  // Parse/validate the source outside the callback lock.
  if(!DspVoiceMixer::Prepare(source,passes,48000,gain,pitch,voice,error))return 0;
  voice.admission=admission;
  SDL_LockAudioDevice(state_->device);const auto id=state_->mixer.Add(std::move(voice));SDL_UnlockAudioDevice(state_->device);
  if(!id)error="Native DSP voice capacity reached";else error.clear();return id;
}
bool DspAudioOutput::PlayBatch(std::span<const PlayRequest> requests,std::vector<uint64_t>& ids,std::string& error){
  if(!state_){error="Native DSP audio output is closed";return false;}
  if(requests.empty() || requests.size()>64){error="Invalid native DSP event voice count";return false;}
  std::vector<DspVoiceMixer::Voice> voices;voices.reserve(requests.size());
  // All source validation/initial decoding precedes the device lock and the
  // first mixer mutation. Encoded source owners remain pinned by the readers.
  for(const auto& request:requests){
    DspVoiceMixer::Voice voice;
    if(!DspVoiceMixer::Prepare(request.source,request.passes,48000,request.gain,request.pitch,voice,error))return false;
    voice.admission=request.admission;voice.delay_frames=request.delay_frames;voice.modulation=request.modulation;voices.push_back(std::move(voice));
  }
  SDL_LockAudioDevice(state_->device);
  const auto accepted=state_->mixer.AddBatch(voices,ids,error);
  SDL_UnlockAudioDevice(state_->device);return accepted;
}
bool DspAudioOutput::Stop(uint64_t id){if(!state_)return false;SDL_LockAudioDevice(state_->device);const auto result=state_->mixer.Stop(id);SDL_UnlockAudioDevice(state_->device);return result;}
size_t DspAudioOutput::Active(){if(!state_)return 0;SDL_LockAudioDevice(state_->device);const auto count=state_->mixer.Active();SDL_UnlockAudioDevice(state_->device);return count;}
uint64_t DspAudioOutput::CallbackCount()const{return state_?state_->callbacks.load():0;}
}
