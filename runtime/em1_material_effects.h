#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace em2::em1 {
enum SimpleMaterialEffects : uint32_t {
  kSimpleAlphaEffect=1, kSimpleGreater127Effect=2, kSimpleGequal127Effect=4,
  kMaskedGreater127Effect=8,
  kTemplateOpaqueEffect=16,kTemplateAlphaEffect=32,kTemplateReadOpaqueEffect=64,
  kOriginalInkRibbonEffect=128
};
// Exact audited property cases only. PE0 never gains a cutout/depth template;
// unknown thresholds/comparisons and multiple NiAlpha properties stay native.
inline const char* SelectSimpleMaterialEffect(uint32_t pe,uint32_t alpha_count,
    uint16_t alpha_flags,uint8_t threshold,uint32_t available) {
  if(alpha_count!=1)return nullptr;
  if(pe==4 && threshold==127){
    if(alpha_flags==0x12EC && (available&kSimpleGreater127Effect))return "EM1G_StaticEnv_Blend";
    if(alpha_flags==0x1AEC && (available&kSimpleGequal127Effect))return "EM1Q_StaticEnv_Blend";
  }
  if((pe==0 || pe==4) && (alpha_flags&1) && !(alpha_flags&0x0200) && (available&kSimpleAlphaEffect))
    return "EM1S_StaticEnv_Simple";
  return nullptr;
}
inline const char* SelectMaskedMaterialEffect(uint32_t pe,uint32_t alpha_count,
    uint16_t flags,uint8_t threshold,uint32_t available) {
  return pe==4 && alpha_count==1 && flags==0x12EC && threshold==127 &&
      (available&kMaskedGreater127Effect)?"EM1H_StaticEnv_Blend":nullptr;
}
// Only the actual mounted PAK directory authorizes a generated alias. This
// does not assert native FXLO linking or shader execution; preparation's GPU
// validation is a separate requirement. Malformed/duplicate entries fail shut.
inline uint32_t ReadSimpleMaterialEffectDirectory(std::span<const uint8_t> bytes,size_t file_size) {
  try {
    auto require=[](bool ok){if(!ok)throw false;};
    require(bytes.size()>=32 && file_size>=bytes.size());
    const bool big=bytes[0]==' ' && bytes[1]=='K' && bytes[2]=='A' && bytes[3]=='P';
    require(big || (bytes[0]=='P' && bytes[1]=='A' && bytes[2]=='K' && bytes[3]==' '));
    auto word=[&](size_t at){require(at<=bytes.size() && bytes.size()-at>=4);uint32_t v=0;
      for(unsigned i=0;i<4;++i)v|=uint32_t(bytes[at+i])<<(8*(big?3-i:i));return v;};
    require(word(4)==2 && word(8)==0);
    const size_t header=word(12),payload=header+size_t(word(16));
    require(header>=32 && payload<=bytes.size() && payload<=16*1024*1024);
    const size_t count=word(header),strings=header+4+count*24;
    require(count<=20000 && strings<=payload);
    auto text=[&](uint32_t offset){require(offset<payload-strings);const size_t start=strings+offset;
      size_t end=start;while(end<payload && bytes[end]){require(end-start<256);++end;}
      require(end<payload);std::string result(reinterpret_cast<const char*>(bytes.data()+start),end-start);
      for(auto& c:result){if(c=='\\')c='/';if(c>='A'&&c<='Z')c+=32;}return result;};
    uint32_t found=0;size_t data_at=payload;
    for(size_t i=0;i<count;++i){const auto at=header+4+i*24;const auto size=word(at),stored=word(at+4),aligned=word(at+8);
      require(aligned%32==0 && stored<=aligned && data_at<=file_size && aligned<=file_size-data_at);
      auto path=text(word(at+12));if(!path.empty())path+='/';path+=text(word(at+20));
      uint32_t bit=0;
      if(path=="cookedshaders/360/em1s_staticenv_simple.fxlo")bit=kSimpleAlphaEffect;
      if(path=="cookedshaders/360/em1g_staticenv_blend.fxlo")bit=kSimpleGreater127Effect;
      if(path=="cookedshaders/360/em1q_staticenv_blend.fxlo")bit=kSimpleGequal127Effect;
      if(path=="cookedshaders/360/em1h_staticenv_blend.fxlo")bit=kMaskedGreater127Effect;
      if(path=="cookedshaders/360/em1o_defaultshadertextured.fxlo")bit=kTemplateOpaqueEffect;
      if(path=="cookedshaders/360/em1t_defaultshadertextured.fxlo")bit=kTemplateAlphaEffect;
      if(path=="cookedshaders/360/em1r_defaultshadertextured.fxlo")bit=kTemplateReadOpaqueEffect;
      if(path=="cookedshaders/360/em1i_fx_env_mask_vertalpha.fxlo")bit=kOriginalInkRibbonEffect;
      if(bit){require(size && stored && word(at+16)==0 && !(found&bit));found|=bit;}
      data_at+=aligned;
    }
    return found;
  }catch(...){return 0;}
}

inline bool TestSimpleMaterialEffectDirectory(std::string& error) {
  for(uint32_t pe:{0u,4u,7u})for(uint32_t mask=0;mask<16;++mask)
    for(uint16_t flags:{uint16_t(0x12EC),uint16_t(0x1AEC)})for(uint8_t threshold:{uint8_t(126),uint8_t(127),uint8_t(128)}){
      const auto* selected=SelectSimpleMaterialEffect(pe,1,flags,threshold,mask);
      const auto bit=flags==0x12EC?kSimpleGreater127Effect:kSimpleGequal127Effect;
      if(bool(selected)!=(pe==4 && threshold==127 && bool(mask&bit))){error="Unaudited cutout alias selection";return false;}
      if(SelectSimpleMaterialEffect(pe,2,flags,threshold,mask)){error="Multiple alpha property precedence guessed";return false;}
      const auto* masked=SelectMaskedMaterialEffect(pe,1,flags,threshold,mask);
      if(bool(masked)!=(pe==4 && flags==0x12EC && threshold==127 && bool(mask&8)) ||
          SelectMaskedMaterialEffect(pe,2,flags,threshold,mask)){
        error="Unaudited masked cutout alias selection";return false;
      }
    }
  for(bool big:{false,true})for(unsigned variant=0;variant<8;++variant){
    std::vector<uint8_t> bytes(60,0);const char* magic=big?" KAP":"PAK ";
    for(unsigned i=0;i<4;++i)bytes[i]=magic[i];
    auto word=[&](size_t at,uint32_t v){for(unsigned i=0;i<4;++i)bytes[at+i]=uint8_t(v>>(8*(big?3-i:i)));};
    word(4,2);word(12,32);word(32,1);word(36,1);word(40,1);word(44,32);
    const std::array names={"EM1S_StaticEnv_Simple.fxlo","EM1G_StaticEnv_Blend.fxlo","EM1Q_StaticEnv_Blend.fxlo","EM1H_StaticEnv_Blend.fxlo","EM1O_DefaultShaderTextured.fxlo","EM1T_DefaultShaderTextured.fxlo","EM1R_DefaultShaderTextured.fxlo","EM1I_FX_Env_Mask_VertAlpha.fxlo"};
    const std::string folder="cookedshaders/360",name=names[variant];
    bytes.insert(bytes.end(),folder.begin(),folder.end());bytes.push_back(0);word(56,uint32_t(folder.size()+1));
    bytes.insert(bytes.end(),name.begin(),name.end());bytes.push_back(0);
    const auto payload=bytes.size();word(16,uint32_t(payload-32));
    if(ReadSimpleMaterialEffectDirectory(bytes,payload+32)!=(1u<<variant)){error="Mounted one-map alias not recognized";return false;}
    auto missing=bytes;missing[60]='X';if(ReadSimpleMaterialEffectDirectory(missing,payload+32)){error="Missing alias activated";return false;}
    auto malformed=bytes;malformed.resize(payload-1);if(ReadSimpleMaterialEffectDirectory(malformed,payload+32)){error="Truncated alias directory activated";return false;}
    if(ReadSimpleMaterialEffectDirectory(bytes,payload+31)){error="Truncated alias payload activated";return false;}
    auto duplicate=bytes;const std::vector<uint8_t> record(bytes.begin()+36,bytes.begin()+60);
    duplicate.insert(duplicate.begin()+60,record.begin(),record.end());
    for(unsigned i=0;i<4;++i){duplicate[32+i]=uint8_t(2u>>(8*(big?3-i:i)));duplicate[16+i]=uint8_t(uint32_t(payload+24-32)>>(8*(big?3-i:i)));}
    if(ReadSimpleMaterialEffectDirectory(duplicate,payload+24+64)){error="Duplicate alias activated";return false;}
    word(40,33);if(ReadSimpleMaterialEffectDirectory(bytes,payload+32)){error="Invalid alias extent activated";return false;}
  }
  error.clear();return true;
}
}
