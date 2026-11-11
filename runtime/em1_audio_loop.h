#pragma once
#include "em1_audio_source.h"
namespace em2::em1 {
// Sequential voice decoder. Encoded bytes stay owned by the source bank; PCM
// storage is supplied by the output mixer. Zero passes means indefinite looping.
class WiiDspVoiceReader {
  std::shared_ptr<const std::vector<uint8_t>> owner_;
  WiiDspDefinition source_;
  uint32_t position_=0,start_=0,end_=0;
  uint16_t passes_=1;
  int64_t h1_=0,h2_=0;
  bool loop_control_=false;
 public:
  bool Begin(const WiiEmbeddedDspSound& source,uint16_t passes,std::string& error){
    try {
      WiiDspVoiceReader candidate;
      if(!source.owner)throw std::runtime_error("DSP voice has no source owner");
      const auto base=reinterpret_cast<uintptr_t>(source.owner->data()),data=reinterpret_cast<uintptr_t>(source.media.data());
      if(data<base || data-base>source.owner->size() || source.media.size()>source.owner->size()-(data-base))throw std::runtime_error("DSP voice media is not owned by its source");
      if(!ReadWiiDspDefinition(source.media,candidate.source_,error))return false;
      if(!candidate.source_.sample_count || candidate.source_.sample_rate!=source.sample_rate)throw std::runtime_error("Invalid DSP voice duration/rate");
      candidate.owner_=source.owner;candidate.passes_=passes;candidate.h1_=candidate.source_.history[0];candidate.h2_=candidate.source_.history[1];
      if(passes!=1){
        const auto sampler=candidate.source_.sampler;
        using audio_source_detail::Word;
        if(Word(sampler,28)==0)throw std::runtime_error("DSP loop has no authored range");
        const uint64_t offset=Word(sampler,32);
        if(offset+52>sampler.size())throw std::runtime_error("Truncated DSP loop range");
        candidate.start_=Word(sampler,size_t(offset)+44);candidate.end_=Word(sampler,size_t(offset)+48);
        if(candidate.start_>=candidate.end_ || candidate.end_>candidate.source_.sample_count || candidate.source_.loop_context[0]>127)throw std::runtime_error("Invalid DSP loop range/context");
      }
      for(size_t i=0;i<(candidate.source_.sample_count+13)/14;++i)if(candidate.source_.data[i*8]>>4>=8)throw std::runtime_error("Invalid DSP predictor");
      *this=std::move(candidate);error.clear();return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
  }
  size_t Read(std::span<int16_t> output){
    if(!owner_)return 0;size_t written=0;
    while(written<output.size()){
      if(passes_!=1 && position_==end_){
        if(passes_>1)--passes_;position_=start_;h1_=int16_t(source_.loop_context[1]);h2_=int16_t(source_.loop_context[2]);loop_control_=true;
      }
      if(position_>=source_.sample_count)break;
      const auto frame=position_/14*8,index=position_%14;
      const auto control=loop_control_?uint8_t(source_.loop_context[0]):source_.data[frame];
      const auto predictor=control>>4;const int64_t scale=int64_t(1)<<(control&15);
      const auto packed=source_.data[frame+1+index/2];int nibble=(index&1)?packed&15:packed>>4;if(nibble>=8)nibble-=16;
      const int64_t value=(int64_t(nibble)*scale*2048+1024+source_.coefficients[predictor*2]*h1_+source_.coefficients[predictor*2+1]*h2_)>>11;
      const auto sample=int16_t(std::clamp<int64_t>(value,-32768,32767));output[written++]=sample;h2_=h1_;h1_=sample;
      if(++position_%14==0)loop_control_=false;
    }
    return written;
  }
};
}
