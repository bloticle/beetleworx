#pragma once
#include "em1_nif_container.h"
namespace em2::em1 {
// Some retail EM1 external images are standard NiPixelData roots, already in
// linear BC storage. Keep their original compressed mips and face ordering;
// these are not Wii tiled persistent-renderer payloads.
inline bool ValidatePortableBcPixels(nif::View bytes,bool big,std::string& error){
  try {
    using namespace nif;
    Check(bytes.size()>=91 && bytes.size()<=kMaximum,"Invalid portable BC pixel bounds");
    const auto format=U32(bytes,0,big);
    Check(format>=4 && format<=6 && bytes[4]==0 && (bytes[13]&1) && U32(bytes,14,big)==0,
          "Unsupported portable BC format/byte order/tiling");
    Check(U32(bytes,59,big)==UINT32_MAX && U32(bytes,67,big)==0,"Invalid portable BC palette/stride");
    const auto count=U32(bytes,63,big);Check(count>=1 && count<=16,"Invalid portable BC mip count");
    const size_t end=71+size_t(count)*12;
    Check(end<=bytes.size() && bytes.size()-end>=8,"Truncated portable BC mip table");
    const auto face_bytes=U32(bytes,end,big),faces=U32(bytes,end+4,big);
    Check((faces==1 || faces==6) && uint64_t(face_bytes)*faces==bytes.size()-end-8,"Invalid portable BC face payload");
    const uint64_t block_bytes=format==4?8:16;
    const auto width=U32(bytes,71,big),height=U32(bytes,75,big);
    Check(width && height && width<=8192 && height<=8192,"Invalid portable BC image dimensions");
    for(size_t i=0;i<count;++i){const auto at=71+i*12;
      const auto w=U32(bytes,at,big),h=U32(bytes,at+4,big),offset=U32(bytes,at+8,big);
      const auto next=i+1<count?U32(bytes,at+20,big):face_bytes;
      const uint64_t size=uint64_t((w+3)/4)*((h+3)/4)*block_bytes;
      Check(w==std::max(1u,width>>i) && h==std::max(1u,height>>i) &&
            (!i?offset==0:true) && uint64_t(offset)+size<=next && next<=face_bytes,
            "Invalid portable BC mip extent/offset");
    }
    error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
// Validate the exact portable NiPixelData emitted by AdaptNifTextures. This
// payload has no source user-version fields; source texture objects stay local.
inline bool ValidatePortableRgbaPixels(nif::View bytes,bool big,std::string& error){
  try {
    using namespace nif;
    Check(bytes.size()>=79 && bytes.size()<=kMaximum,"Invalid portable pixel bounds");
    Check(U32(bytes,0,big)==1 && bytes[4]==32 && U32(bytes,5,big)==UINT32_MAX && U32(bytes,9,big)==0 && bytes[13]==1 && U32(bytes,14,big)==0,"Invalid portable RGBA pixel layout");
    for(size_t channel=0;channel<4;++channel){const auto at=19+channel*10;
      Check(U32(bytes,at,big)==channel && U32(bytes,at+4,big)==0 && bytes[at+8]==8 && bytes[at+9]==0,"Invalid portable RGBA channel descriptor");}
    Check(U32(bytes,59,big)==UINT32_MAX && U32(bytes,67,big)==4,"Portable pixels retain palette/invalid stride");
    const auto count=U32(bytes,63,big);Check(count>=1 && count<=16,"Invalid portable mip count");
    const size_t end=71+size_t(count)*12;
    Check(end<=bytes.size() && bytes.size()-end>=8 && U32(bytes,end+4,big)==1,"Invalid portable pixel face metadata");
    uint64_t expanded=0;
    for(size_t i=0;i<count;++i){const auto at=71+i*12;const auto width=U32(bytes,at,big),height=U32(bytes,at+4,big),offset=U32(bytes,at+8,big);
      Check(width && height && width<=8192 && height<=8192 && offset==expanded,"Invalid portable mip dimensions/offset");
      expanded+=uint64_t(width)*height*4;Check(expanded<=kMaximum,"Portable mip budget exceeded");}
    Check(expanded==U32(bytes,end,big) && expanded==bytes.size()-end-8,"Portable pixel data length mismatch");
    error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
inline bool ValidatePortablePixels(nif::View bytes,bool big,std::string& error){
  return bytes.size()>=4 && nif::U32(bytes,0,big)>=4 && nif::U32(bytes,0,big)<=6?
      ValidatePortableBcPixels(bytes,big,error):ValidatePortableRgbaPixels(bytes,big,error);
}
inline bool TestPortableBcPixels(std::string& error){
  try {
    using namespace nif;
    for(bool big:{false,true})for(uint32_t format:{4u,5u,6u})for(uint32_t faces:{1u,6u}){
      // Authored 8x4, 4x2, 2x1, 1x1 chain, including sub-block mips.
      const uint32_t block=format==4?8:16,face_bytes=5*block;
      Bytes pixels(127+uint64_t(face_bytes)*faces,0);
      auto word=[&](size_t at,uint32_t value){for(unsigned j=0;j<4;++j)pixels.at(at+j)=uint8_t(value>>(8*(big?3-j:j)));};
      word(0,format);pixels[13]=1;word(59,UINT32_MAX);word(63,4);word(119,face_bytes);word(123,faces);
      uint32_t offset=0;
      for(unsigned i=0;i<4;++i){word(71+i*12,std::max(1u,8u>>i));word(75+i*12,std::max(1u,4u>>i));word(79+i*12,offset);offset+=(i?1:2)*block;}
      Check(ValidatePortableBcPixels(pixels,big,error),"Valid standard BC mip/face layout rejected");
      auto truncated=pixels;truncated.pop_back();Check(!ValidatePortableBcPixels(truncated,big,error),"Truncated BC face accepted");
      word(99,face_bytes+1);Check(!ValidatePortableBcPixels(pixels,big,error),"Oversized BC mip accepted");word(99,1);
      word(63,17);Check(!ValidatePortableBcPixels(pixels,big,error),"Oversized BC mip table accepted");word(63,4);
      word(123,UINT32_MAX);Check(!ValidatePortableBcPixels(pixels,big,error),"Overflowing BC face count accepted");word(123,faces);
      pixels[13]=0;Check(!ValidatePortableBcPixels(pixels,big,error),"Unaudited BC byte order accepted");
    }
    error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
}
