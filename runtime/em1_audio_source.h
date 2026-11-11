#pragma once
#include "em1_audio_bank.h"
#include "em1_audio_dsp.h"
#include <array>
namespace em2::em1 {
namespace audio_source_detail {
inline std::span<const uint8_t> Object(const WiiAudioBank& bank,uint32_t kind,uint32_t id){
  const WiiAudioBank::Object* found=nullptr;
  for(const auto& value:bank.objects)if(value.kind==kind && value.id==id){if(found)throw std::runtime_error("Ambiguous typed audio object");found=&value;}
  if(!found || !bank.bytes || uint64_t(found->offset)+found->size>bank.bytes->size())throw std::runtime_error("Missing/invalid typed audio object");
  return std::span(*bank.bytes).subspan(found->offset,found->size);
}
inline uint32_t Word(std::span<const uint8_t> b,size_t at){
  if(at>b.size() || b.size()-at<4)throw std::runtime_error("Truncated audio source/action");
  return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];
}
}
struct WiiAudioAction {
  std::shared_ptr<const std::vector<uint8_t>> owner;
  uint32_t kind=0,target=0,subsection_size=0;
  std::array<int32_t,3> delay{};
  std::span<const uint8_t> parameters;
};
inline bool ReadWiiAudioAction(const WiiAudioBank& bank,uint32_t id,WiiAudioAction& output,std::string& error){
  using namespace audio_source_detail;
  try {
    const auto b=Object(bank,3,id);WiiAudioAction result;result.owner=bank.bytes;
    result.kind=Word(b,0);result.target=Word(b,4);for(size_t i=0;i<3;++i)result.delay[i]=int32_t(Word(b,8+i*4));
    result.subsection_size=Word(b,20);result.parameters=b.subspan(24);
    if(result.subsection_size>result.parameters.size())throw std::runtime_error("Truncated action subsection");
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
struct WiiEmbeddedDspSound {
  std::shared_ptr<const std::vector<uint8_t>> owner;
  uint32_t media_id=0,bank_id=0,sample_rate=0,format_bits=0;
  uint8_t flags=0;
  std::span<const uint8_t> media,node_parameters;
};
// Resolves the exact authored media range, not a similarly named sound. Unknown
// stream/plugin modes require their own readers; node settings remain intact.
inline bool ResolveWiiEmbeddedDspSound(const WiiAudioBank& bank,uint32_t id,WiiEmbeddedDspSound& output,std::string& error){
  using namespace audio_source_detail;
  try {
    const auto b=Object(bank,2,id);
    if(Word(b,0)!=0x00050001 || Word(b,4)!=0)throw std::runtime_error("Sound is not an embedded Wii DSP source");
    WiiEmbeddedDspSound result;result.owner=bank.bytes;result.sample_rate=Word(b,8);result.format_bits=Word(b,12);
    result.media_id=Word(b,16);result.bank_id=Word(b,20);
    const auto offset=Word(b,24),size=Word(b,28);
    if(b.size()<33 || result.bank_id!=bank.id)throw std::runtime_error("Source bank/flags unavailable");
    result.flags=b[32];result.node_parameters=b.subspan(33);
    const WiiAudioBank::Media* found=nullptr;
    for(const auto& media:bank.media)if(media.id==result.media_id){if(found)throw std::runtime_error("Ambiguous sound media");found=&media;}
    if(!found || found->offset!=offset || found->size!=size || uint64_t(offset)+size>bank.bytes->size())throw std::runtime_error("Source range differs from bank media identity");
    result.media=std::span(*bank.bytes).subspan(offset,size);output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
inline bool DecodeWiiEmbeddedDspSound(const WiiAudioBank& bank,uint32_t id,NativeAudioPcm& output,std::string& error){
  WiiEmbeddedDspSound source;if(!ResolveWiiEmbeddedDspSound(bank,id,source,error))return false;
  NativeAudioPcm result;if(!DecodeWiiDspMedia(source.media,result,error))return false;
  if(result.sample_rate!=source.sample_rate){error="Sound and media sample rates differ";return false;}
  output=std::move(result);return true;
}
}
