#pragma once
#include "em1_audio_loop.h"
#include "em1_audio_admission.h"
#include <cmath>
#include <limits>
namespace em2::em1 {
class DspVoiceModulation {
 public:
  virtual ~DspVoiceModulation()=default;
  // Audio callback only. Prepared implementations must not allocate, access
  // guest memory, acquire game locks or perform file I/O here.
  virtual bool GainAt(uint64_t elapsed_frames,std::array<float,2>& gain) noexcept=0;
};
// Control calls and Render must be serialized by the owning output device.
class DspVoiceMixer {
 public:
  struct Voice {
    WiiDspVoiceReader reader;
    double phase=0,step=1;
    std::array<float,2> gain{1,1};
    int16_t a=0,b=0;
    bool last=false,active=false;
    uint64_t id=0;
    WiiVoiceAdmission admission;
    bool stopping=false;
    uint64_t elapsed_frames=0,delay_frames=0;
    std::shared_ptr<DspVoiceModulation> modulation;
  };
 private:
  std::array<Voice,64> voices_;
  uint64_t next_id_=0;
 public:
  static bool Prepare(const WiiEmbeddedDspSound& source,uint16_t passes,uint32_t output_rate,
      std::array<float,2> gain,double pitch,Voice& output,std::string& error){
    if(!output_rate || !std::isfinite(pitch) || pitch<=0 || pitch>16 ||
      !std::isfinite(gain[0]) || !std::isfinite(gain[1]) || std::abs(gain[0])>64 || std::abs(gain[1])>64){error="Invalid voice mix settings";return false;}
    Voice candidate;if(!candidate.reader.Begin(source,passes,error))return false;
    candidate.gain=gain;candidate.step=double(source.sample_rate)/output_rate*pitch;
    if(candidate.reader.Read(std::span(&candidate.a,1))!=1){error="Empty DSP voice";return false;}
    candidate.last=candidate.reader.Read(std::span(&candidate.b,1))==0;
    if(candidate.last)candidate.b=candidate.a;
    candidate.active=true;output=std::move(candidate);error.clear();return true;
  }
  uint64_t Add(Voice voice){
    const auto& policy=voice.admission;
    if(!voice.active || !std::isfinite(policy.priority) || policy.lineage_size>64 || next_id_==UINT64_MAX)return 0;
    if(policy.node && std::find(policy.lineage.begin(),policy.lineage.begin()+policy.lineage_size,policy.node)==policy.lineage.begin()+policy.lineage_size)return 0;
    Voice* selected=nullptr;
    if(policy.maximum){
      std::array<WiiVoiceCandidate,64> candidates;size_t count=0,eligible=0;
      for(const auto& slot:voices_)if(slot.active){
        const bool match=!policy.node || std::find(slot.admission.lineage.begin(),slot.admission.lineage.begin()+slot.admission.lineage_size,policy.node)!=slot.admission.lineage.begin()+slot.admission.lineage_size;
        candidates[count++]={slot.id,slot.admission.owner,slot.admission.priority,match,slot.stopping};
        eligible+=!slot.stopping && (!policy.owner || slot.admission.owner==policy.owner) && match;
      }
      // State-driven shrinking needs its own retirement path; do not admit
      // more voices into a scope already above its new limit.
      if(eligible>policy.maximum)return 0;
      if(eligible==policy.maximum){
        std::sort(candidates.begin(),candidates.begin()+count,[](const auto& a,const auto& b){return a.id<b.id;});
        const auto victim=WiiSelectVoiceVictim(policy.priority,policy.kill_newest,std::span(candidates.data(),count),policy.owner,policy.node);
        if(!victim)return 0;for(auto& slot:voices_)if(slot.id==victim){selected=&slot;break;}
      }
    }
    if(!selected)for(auto& slot:voices_)if(!slot.active){selected=&slot;break;}
    if(!selected)return 0;
    voice.id=++next_id_;*selected=std::move(voice);return selected->id;
  }
  // An event can start several voices. Admission (including stealing) is
  // transactional, so a later invalid/capacity-limited voice cannot leave
  // half an authored event playing or retire a pre-existing voice.
  bool AddBatch(std::span<const Voice> voices,std::vector<uint64_t>& output,std::string& error){
    if(voices.empty() || voices.size()>voices_.size()){error="Invalid native DSP event voice count";return false;}
    auto candidate=*this;std::vector<uint64_t> ids;ids.reserve(voices.size());
    for(const auto& voice:voices){
      const auto id=candidate.Add(voice);
      if(!id){error="Native DSP event admission rejected";return false;}
      ids.push_back(id);
    }
    // A later voice may steal an earlier voice in the same authored event.
    // This API promises admission of the whole batch, rather than returning
    // playing IDs that have already disappeared before the event starts.
    for(const auto id:ids)if(!candidate.Contains(id)){error="Native DSP event voices compete for the same limit";return false;}
    *this=std::move(candidate);output=std::move(ids);error.clear();return true;
  }
  bool Stop(uint64_t id){for(auto& voice:voices_)if(voice.id==id && id){voice=Voice{};return true;}return false;}
  bool Contains(uint64_t id)const{for(const auto& voice:voices_)if(voice.id==id && id && voice.active)return true;return false;}
  size_t Active()const{size_t count=0;for(const auto& voice:voices_)count+=voice.active;return count;}
  void Render(std::span<float> stereo){
    std::fill(stereo.begin(),stereo.end(),0.f);
    for(auto& voice:voices_)if(voice.active){
      for(size_t at=0;at+1<stereo.size() && voice.active;at+=2){
        const auto elapsed=voice.elapsed_frames++;
        if(elapsed<voice.delay_frames)continue;
        auto gain=voice.gain;
        if(voice.modulation && !voice.modulation->GainAt(elapsed-voice.delay_frames,gain)){voice.active=false;break;}
        const auto sample=float((voice.a+(voice.b-voice.a)*voice.phase)/32768.0);
        stereo[at]+=sample*gain[0];stereo[at+1]+=sample*gain[1];
        voice.phase+=voice.step;
        while(voice.phase>=1 && voice.active){
          voice.phase-=1;
          if(voice.last){voice.active=false;break;}
          voice.a=voice.b;voice.last=voice.reader.Read(std::span(&voice.b,1))==0;
          if(voice.last)voice.b=voice.a;
        }
      }
    }
    for(auto& value:stereo)value=std::clamp(value,-1.f,1.f);
    // Finished readers retain bank ownership until the control thread reuses
    // or stops the slot, avoiding destruction/deallocation in the callback.
  }
};
inline bool TestAtomicDspEventAdmission(){
  auto voice=[](float gain=1.f){DspVoiceMixer::Voice v;v.active=true;v.a=v.b=8192;v.last=true;v.step=.25;v.gain={gain,gain};return v;};
  std::string error;std::vector<uint64_t> ids{999};
  DspVoiceMixer full;
  for(size_t i=0;i<63;++i)if(!full.Add(voice(.01f)))return false;
  auto unchanged=full;std::array batch{voice(),voice()};
  if(full.AddBatch(batch,ids,error) || ids!=std::vector<uint64_t>{999} || full.Active()!=63)return false;
  std::array<float,16> actual{},expected{};full.Render(actual);unchanged.Render(expected);
  if(actual!=expected || actual.front()==0 || full.Active()!=unchanged.Active())return false;
  // A candidate would steal the existing voice, then the malformed second
  // candidate fails. Neither that retirement nor its ID is committed.
  DspVoiceMixer limited;auto old=voice(.5f);old.admission.maximum=1;
  const auto old_id=limited.Add(old);unchanged=limited;
  batch={voice(),voice()};batch[0].admission.maximum=1;batch[0].admission.priority=90;
  batch[1].admission.priority=std::numeric_limits<float>::quiet_NaN();
  if(limited.AddBatch(batch,ids,error) || ids!=std::vector<uint64_t>{999} || !limited.Active())return false;
  limited.Render(actual);unchanged.Render(expected);if(actual!=expected || actual.front()!=.125f)return false;
  if(!limited.Stop(old_id))return false;
  batch={voice(),voice()};for(auto& v:batch){v.admission.maximum=1;v.admission.priority=90;}
  if(limited.AddBatch(batch,ids,error) || limited.Active()!=0 || ids!=std::vector<uint64_t>{999})return false;
  batch={voice(.25f),voice(.5f)};
  if(!limited.AddBatch(batch,ids,error) || ids!=std::vector<uint64_t>{old_id+1,old_id+2} || limited.Active()!=2)return false;
  limited.Render(actual);if(actual.front()!=.1875f)return false;
  for(auto id:ids)if(!limited.Stop(id))return false;
  if(limited.Active())return false;
  limited.Render(actual);if(std::any_of(actual.begin(),actual.end(),[](float sample){return sample!=0;}))return false;
  return true;
}
}
