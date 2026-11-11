#pragma once
#include "em1_audio_mixer.h"
namespace em2::em1 {
class DspAudioOutput {
  struct State;std::unique_ptr<State> state_;
 public:
  struct PlayRequest {
    WiiEmbeddedDspSound source;
    uint16_t passes=1;
    std::array<float,2> gain{1,1};
    double pitch=1;
    WiiVoiceAdmission admission;
    uint64_t delay_frames=0;
    std::shared_ptr<DspVoiceModulation> modulation;
  };
  DspAudioOutput();~DspAudioOutput();
  DspAudioOutput(const DspAudioOutput&)=delete;
  DspAudioOutput& operator=(const DspAudioOutput&)=delete;
  bool Open(std::string& error);
  uint64_t Play(const WiiEmbeddedDspSound&,uint16_t passes,std::array<float,2> gain,double pitch,std::string& error,const WiiVoiceAdmission& admission={});
  bool PlayBatch(std::span<const PlayRequest>,std::vector<uint64_t>& ids,std::string& error);
  bool Stop(uint64_t id);
  size_t Active();
  uint64_t CallbackCount()const;
};
}
