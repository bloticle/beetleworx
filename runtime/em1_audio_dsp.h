#pragma once
#include <array>
#include <algorithm>
#include <map>
#include <span>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>
namespace em2::em1 {
struct NativeAudioPcm {
  uint32_t sample_rate=0;
  std::vector<int16_t> samples;
  std::vector<uint8_t> sampler;
  std::array<uint16_t,3> loop_context{};
};
// Native mono Nintendo DSP conversion. Preserves sampler/context metadata;
// event scheduling, looping, spatialization and bank graph conversion follow.
struct WiiDspDefinition {
  uint32_t sample_rate=0,sample_count=0;
  std::array<int16_t,16> coefficients{};
  std::array<int16_t,2> history{};
  std::array<uint16_t,3> loop_context{};
  std::span<const uint8_t> data,sampler;
};
inline bool ReadWiiDspDefinition(std::span<const uint8_t> bytes,WiiDspDefinition& output,std::string& error){
  try {
    auto check=[](bool value,const char* why){if(!value)throw std::runtime_error(why);};
    auto word=[&](std::span<const uint8_t> b,size_t at){check(at<=b.size() && b.size()-at>=4,"Truncated audio integer");return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];};
    auto half=[&](std::span<const uint8_t> b,size_t at){check(at<=b.size() && b.size()-at>=2,"Truncated audio halfword");return uint16_t(uint16_t(b[at])<<8|b[at+1]);};
    auto tag=[&](std::span<const uint8_t> b,size_t at){check(at<=b.size() && b.size()-at>=4,"Truncated audio tag");return std::string(reinterpret_cast<const char*>(b.data()+at),4);};
    check(bytes.size()>=12 && bytes.size()<=256ull*1024*1024 && tag(bytes,0)=="RIFX" && tag(bytes,8)=="WAVE","Expected bounded Wii RIFX/WAVE");
    check(uint64_t(word(bytes,4))+8<=bytes.size(),"Truncated declared audio container");
    std::map<std::string,std::span<const uint8_t>> chunks;
    for(size_t at=12;at<bytes.size();){
      const auto name=tag(bytes,at);const auto size=word(bytes,at+4);
      check(bytes.size()-at>=8 && size<=bytes.size()-at-8,"Truncated audio chunk");
      check(chunks.emplace(name,bytes.subspan(at+8,size)).second,"Duplicate audio chunk");
      at+=8+size;if(at<bytes.size())at+=size&1;check(at<=bytes.size(),"Missing audio padding");
    }
    check(chunks.contains("fmt ") && chunks.contains("WiiH") && chunks.contains("data"),"Missing DSP audio chunks");
    const auto fmt=chunks.at("fmt "),header=chunks.at("WiiH"),data=chunks.at("data");
    check(fmt.size()==40 && header.size()==46 && half(fmt,0)==0xfff0 && half(fmt,2)==1 && half(fmt,12)==8 && half(fmt,14)==4,"Unsupported Wii DSP layout/codec");
    WiiDspDefinition result;result.sample_rate=word(fmt,4);const auto count=word(fmt,8);result.sample_count=count;
    check(result.sample_rate>0 && result.sample_rate<=384000 && data.size()%8==0 &&
      count<=64ull*1024*1024 && count<=data.size()/8*14,"Invalid DSP rate/sample count");
    for(size_t i=0;i<16;++i)result.coefficients[i]=int16_t(half(header,i*2));
    result.history={int16_t(half(header,36)),int16_t(half(header,38))};
    for(size_t i=0;i<3;++i)result.loop_context[i]=half(header,40+i*2);
    if(chunks.contains("smpl"))result.sampler=chunks.at("smpl");
    result.data=data;
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
inline bool DecodeWiiDspMedia(std::span<const uint8_t> bytes,NativeAudioPcm& output,std::string& error){
  WiiDspDefinition definition;if(!ReadWiiDspDefinition(bytes,definition,error))return false;
  try {
    NativeAudioPcm result;result.sample_rate=definition.sample_rate;result.loop_context=definition.loop_context;
    result.sampler.assign(definition.sampler.begin(),definition.sampler.end());result.samples.reserve(definition.sample_count);
    int64_t h1=definition.history[0],h2=definition.history[1];
    const auto& coefficients=definition.coefficients;const auto data=definition.data;
    for(size_t at=0;result.samples.size()<definition.sample_count;at+=8){
      const auto predictor=data[at]>>4;if(predictor>=8)throw std::runtime_error("Invalid DSP predictor");
      const int64_t scale=int64_t(1)<<(data[at]&15);
      for(size_t i=0;i<14 && result.samples.size()<definition.sample_count;++i){
        const auto packed=data[at+1+i/2];int nibble=(i&1)?packed&15:packed>>4;if(nibble>=8)nibble-=16;
        const int64_t value=(int64_t(nibble)*scale*2048+1024+coefficients[predictor*2]*h1+coefficients[predictor*2+1]*h2)>>11;
        const auto sample=int16_t(std::clamp<int64_t>(value,-32768,32767));result.samples.push_back(sample);h2=h1;h1=sample;
      }
    }
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}

}
