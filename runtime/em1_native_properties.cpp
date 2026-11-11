#include "em1_native_properties.h"
#include "em1_ammo_change.h"
#include <windows.h>
#include <immintrin.h>
#include <bit>
#include <cmath>
#include <cstring>
#include <set>

PPC_EXTERN_FUNC(__imp__sub_826550C8);
PPC_EXTERN_FUNC(__imp__sub_82655058);
PPC_EXTERN_FUNC(__imp__sub_825430D8);
PPC_EXTERN_FUNC(__imp__sub_827DE520);
PPC_EXTERN_FUNC(__imp__sub_827D2488);
PPC_EXTERN_FUNC(__imp__sub_82504410);
PPC_EXTERN_FUNC(__imp__sub_822F1C10);
namespace em2::em1 {
namespace {
struct Context {PPCContext& ctx;PPCContext saved;DWORD error=GetLastError();unsigned csr=_mm_getcsr();Context(PPCContext& c):ctx(c),saved(c){}~Context(){ctx=saved;_mm_setcsr(csr);SetLastError(error);}};
constexpr uint32_t descriptor_size=20,table_size=100,float_table=0x8208EFF0,boxed_table=0x8208F010;
uint32_t DefaultBits(uint8_t* base,uint32_t descriptor){return PPC_LOAD_U32(PPC_LOAD_U32(descriptor)+32);}
uint32_t Allocate(PPCContext& ctx,uint8_t* base,uint32_t size){ctx.r3.u64=size;ctx.r4.u64=0;__imp__sub_826550C8(ctx,base);return ctx.r3.u32;}
}
void CreateNativeFloatDefault(PPCContext& ctx,uint8_t* base){
  const auto table=PPC_LOAD_U32(ctx.r3.u32),bits=DefaultBits(base,ctx.r3.u32),box_table=PPC_LOAD_U32(table+36);uint32_t box=0;
  {Context restore(ctx);__imp__sub_827D2488(ctx,base);box=ctx.r3.u32;if(box){PPC_STORE_U32(box,box_table);PPC_STORE_U32(box+4,bits);}}
  ctx.r3.u64=box;
}
void ReadNativeFloatDefault(PPCContext& ctx,uint8_t* base){PPC_STORE_U32(ctx.r4.u32,DefaultBits(base,ctx.r3.u32));}
void NativeFloatBoxDescriptor(PPCContext& ctx,uint8_t* base){ctx.r3.u64=PPC_LOAD_U32(PPC_LOAD_U32(ctx.r3.u32)+56);}
void CreateNativeArrayDefault(PPCContext& ctx,uint8_t* base){
  const auto table=PPC_LOAD_U32(ctx.r3.u32),factory=PPC_LOAD_U32(table+100),box_table=PPC_LOAD_U32(table+36);uint32_t box=0;
  {Context restore(ctx);PPC_CALL_INDIRECT_FUNC(factory);box=ctx.r3.u32;if(box)PPC_STORE_U32(box,box_table);}
  ctx.r3.u64=box;
}
void WriteNativeStringProperty(PPCContext& ctx,uint8_t* base){
  // Same native interned-string ownership as retail827579F0, for one scalar.
  const auto destination=ctx.r4.u32+4,input=ctx.r5.u32,value=PPC_LOAD_U32(input);
  if(PPC_LOAD_U32(destination)==value)return;
  Context restore(ctx);ctx.r3.u64=input;__imp__sub_82504410(ctx,base);
  ctx.r3.u64=destination;__imp__sub_822F1C10(ctx,base);PPC_STORE_U32(destination,value);
}
void ReleaseNativeFloatSchema(PPCContext& ctx,uint8_t* base,NativeFloatSchema& schema){
  Context restore(ctx);
  for(const auto descriptor:schema.descriptors){ctx.r3.u64=descriptor;ctx.r4.u64=1;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(descriptor)));}
  if(schema.storage){ctx.r3.u64=schema.storage;ctx.r4.u64=schema.storage_size;__imp__sub_82655058(ctx,base);}
  schema.descriptors.clear();schema.storage=0;schema.storage_size=0;
}
bool BuildNativeFloatSchema(PPCContext& ctx,uint8_t* base,std::span<const FloatPropertyDeclaration> declarations,NativeFloatSchema& output,std::string& error){
  if(!base || declarations.empty() || declarations.size()>256 || output.storage || !output.descriptors.empty()){error="Invalid native float schema request";return false;}
  std::set<std::string> names;uint32_t bytes=uint32_t(declarations.size())*table_size;
  for(const auto& entry:declarations){
    if(entry.name.empty() || entry.name.size()>127 || entry.name.find('\0')!=entry.name.npos || !std::isfinite(entry.default_value) || !names.insert(entry.name).second){error="Invalid or duplicate native float property";return false;}
    bytes+=uint32_t(entry.name.size())+1;
  }
  Context restore(ctx);NativeFloatSchema result;result.descriptors.reserve(declarations.size());
  result.storage_size=bytes;result.storage=Allocate(ctx,base,bytes);
  if(!result.storage){error="Native float schema allocation failed";return false;}
  uint32_t string_offset=uint32_t(declarations.size())*table_size;
  for(size_t i=0;i<declarations.size();++i){const auto& entry=declarations[i];const auto table=result.storage+uint32_t(i)*table_size;
    std::memcpy(base+table,base+float_table,32);PPC_STORE_U32(table+4,kFloatDefaultBox);PPC_STORE_U32(table+28,kFloatDefaultBuffer);
    PPC_STORE_U32(table+32,std::bit_cast<uint32_t>(entry.default_value));
    const auto box_table=table+40;PPC_STORE_U32(table+36,box_table);
    std::memcpy(base+box_table,base+boxed_table,56);PPC_STORE_U32(box_table+44,kFloatBoxDescriptor);
    const auto descriptor=Allocate(ctx,base,descriptor_size);
    if(!descriptor){ReleaseNativeFloatSchema(ctx,base,result);error="Native float descriptor allocation failed";return false;}
    std::memset(base+descriptor,0,descriptor_size);PPC_STORE_U32(descriptor,table);PPC_STORE_U32(descriptor+4,PPC_LOAD_U32(0x8203D278));
    PPC_STORE_U32(box_table+56,descriptor);
    PPC_STORE_U32(descriptor+12,0x828AB248);PPC_STORE_U32(descriptor+16,0x827E2BC8);result.descriptors.push_back(descriptor);
    const auto name=result.storage+string_offset;std::memcpy(base+name,entry.name.c_str(),entry.name.size()+1);string_offset+=uint32_t(entry.name.size())+1;
    ctx.r3.u64=name;__imp__sub_825430D8(ctx,base);
    if(ctx.r3.u32<4096){ReleaseNativeFloatSchema(ctx,base,result);error="Native float property name failed";return false;}
    PPC_STORE_U32(descriptor+4,ctx.r3.u32);
  }
  output.descriptors.swap(result.descriptors);output.storage=result.storage;output.storage_size=result.storage_size;
  result.storage=0;result.storage_size=0;error.clear();return true;
}
bool BuildNativeAmmoSchema(PPCContext& ctx,uint8_t* base,NativeFloatSchema& output,std::string& error){
  std::vector<NativePropertyDeclaration> properties;for(const auto& entry:LegacyAmmoProperties())properties.push_back({std::string(entry.name),NativePropertyShape::Float,entry.default_value});
  return BuildNativePropertySchema(ctx,base,properties,output,error);
}
std::span<const NativePropertyDeclaration> NativeInventoryDeclarations(){
  // Original REL 341EF8 declaration order, not authored GSA property order.
  // All fourteen scalar compiled defaults are zero. The three shared source
  // vector initializers establish null data / zero size / zero capacity.
  static const NativePropertyDeclaration properties[]={
    {"Paint"},{"PaintMax"},{"Thinner"},{"ThinnerMax"},
    {"PaintAmmoSegmentAmount"},{"ThinnerAmmoSegmentAmount"},{"ETicket"},{"ETicketMax"},
    {"BigBall"},{"SuperSaturated"},{"ThinnerShockwave"},{"LongRange"},
    {"Sketch",NativePropertyShape::StringArray},{"SketchCount",NativePropertyShape::IntegerArray},
    {"SketchMax",NativePropertyShape::FloatArray},{"Invisible_Ammo"},{"Indelible_Ammo"}};
  return properties;
}
bool BuildNativeInventorySchema(PPCContext& ctx,uint8_t* base,NativeFloatSchema& output,std::string& error){
  return BuildNativePropertySchema(ctx,base,NativeInventoryDeclarations(),output,error);
}
bool BuildNativePropertySchema(PPCContext& ctx,uint8_t* base,std::span<const NativePropertyDeclaration> declarations,NativeFloatSchema& output,std::string& error){
  constexpr uint32_t block_size=104;
  if(!base || declarations.empty() || declarations.size()>256 || output.storage || !output.descriptors.empty()){error="Invalid native property schema request";return false;}
  std::set<std::string> names;uint32_t bytes=uint32_t(declarations.size())*block_size;
  for(const auto& entry:declarations){
    if(entry.name.empty() || entry.name.size()>127 || entry.name.find('\0')!=entry.name.npos || !std::isfinite(entry.default_value) || !names.insert(entry.name).second ||
       unsigned(entry.shape)>unsigned(NativePropertyShape::Boolean) || (entry.shape!=NativePropertyShape::Float && entry.default_value!=0)){
      error="Invalid native property declaration or unsupported nonzero default";return false;}
    bytes+=uint32_t(entry.name.size())+1;
  }
  Context restore(ctx);NativeFloatSchema result;result.descriptors.reserve(declarations.size());
  result.storage_size=bytes;result.storage=Allocate(ctx,base,bytes);
  if(!result.storage){error="Native property schema allocation failed";return false;}
  uint32_t string_offset=uint32_t(declarations.size())*block_size;
  for(size_t i=0;i<declarations.size();++i){const auto& entry=declarations[i];const auto table=result.storage+uint32_t(i)*block_size;
    uint32_t stock_table=float_table,stock_box=boxed_table,setter=0x828AB248,element_setter=0;
    // Array descriptors are 24 bytes, have IsArray at vtable+24 and native
    // vector boxes of 16 bytes. Vtable+8 is NOT the array flag.
    switch(entry.shape){
      case NativePropertyShape::Float:break;
      case NativePropertyShape::FloatArray:stock_table=0x82076080;stock_box=0x82076148;setter=0x827597D8;element_setter=0x827574C8;break;
      case NativePropertyShape::IntegerArray:stock_table=0x820762F0;stock_box=0x820763B8;setter=0x827597F8;element_setter=0x827574F8;break;
      case NativePropertyShape::StringArray:stock_table=0x82076428;stock_box=0x82076538;setter=0x8275ADB0;element_setter=0x827579F0;break;
      case NativePropertyShape::Integer:stock_table=0x8207F160;stock_box=0x8207F180;setter=0x828BE490;break;
      case NativePropertyShape::String:stock_table=0x820776A0;stock_box=0x82077730;setter=kStringPropertyWrite;break;
      case NativePropertyShape::Point3:stock_table=0x82088E70;stock_box=0x82088EC8;setter=0x828AB0A8;break;
      case NativePropertyShape::Boolean:stock_table=0x82075D2C;stock_box=0x82075D74;setter=0x82756F70;break;
    }
    const bool array=entry.shape==NativePropertyShape::FloatArray || entry.shape==NativePropertyShape::IntegerArray || entry.shape==NativePropertyShape::StringArray;
    std::memcpy(base+table,base+stock_table,32);
    PPC_STORE_U32(table+100,PPC_LOAD_U32(stock_table+4));PPC_STORE_U32(table+4,entry.shape==NativePropertyShape::Float?kFloatDefaultBox:kArrayDefaultBox);
    if(entry.shape==NativePropertyShape::Float)PPC_STORE_U32(table+28,kFloatDefaultBuffer);
    PPC_STORE_U32(table+32,std::bit_cast<uint32_t>(entry.default_value));PPC_STORE_U32(table+36,table+40);
    std::memcpy(base+table+40,base+stock_box,56);PPC_STORE_U32(table+40+44,kFloatBoxDescriptor);
    const auto size=array?24u:20u,descriptor=Allocate(ctx,base,size);
    if(!descriptor){ReleaseNativeFloatSchema(ctx,base,result);error="Native property descriptor allocation failed";return false;}
    std::memset(base+descriptor,0,size);PPC_STORE_U32(descriptor,table);PPC_STORE_U32(descriptor+4,PPC_LOAD_U32(0x8203D278));
    PPC_STORE_U32(table+96,descriptor);PPC_STORE_U32(descriptor+12,setter);PPC_STORE_U32(descriptor+16,0x827E2BC8);
    if(array)PPC_STORE_U32(descriptor+20,element_setter);
    result.descriptors.push_back(descriptor);
    const auto name=result.storage+string_offset;std::memcpy(base+name,entry.name.c_str(),entry.name.size()+1);string_offset+=uint32_t(entry.name.size())+1;
    ctx.r3.u64=name;__imp__sub_825430D8(ctx,base);
    if(ctx.r3.u32<4096){ReleaseNativeFloatSchema(ctx,base,result);error="Native property name registration failed";return false;}
    PPC_STORE_U32(descriptor+4,ctx.r3.u32);
  }
  output.descriptors.swap(result.descriptors);output.storage=result.storage;output.storage_size=result.storage_size;
  error.clear();return true;
}
bool ProbeNativeBooleanSchema(PPCContext& ctx,uint8_t* base,std::string& error){
  Context restore(ctx);NativeFloatSchema schema;const NativePropertyDeclaration declarations[]={
    {"BranchValue",NativePropertyShape::Boolean},{"RelayDisabled",NativePropertyShape::Boolean}};
  if(!BuildNativePropertySchema(ctx,base,declarations,schema,error))return false;
  const auto scratch=Allocate(ctx,base,4);bool valid=scratch!=0;
  for(auto descriptor:schema.descriptors){if(!valid)break;const auto table=PPC_LOAD_U32(descriptor);
    ctx.r3.u64=descriptor;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+20));valid=std::strcmp(reinterpret_cast<const char*>(base+PPC_LOAD_U32(ctx.r3.u32)),"Boolean")==0 && valid;
    std::array<uint32_t,2> boxes{};
    for(auto& box:boxes){ctx.r3.u64=descriptor;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+4));box=ctx.r3.u32;valid=box && valid;}
    if(valid){
      std::memset(base+scratch,0xA5,4);ctx.r3.u64=descriptor;ctx.r4.u64=scratch;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+28));
      valid=base[scratch]==0 && base[scratch+1]==0xA5 && base[boxes[0]+4]==0 && base[boxes[1]+4]==0 && valid;
      for(uint8_t value:{uint8_t(1),uint8_t(0),uint8_t(1)}){
        base[scratch]=value;ctx.r3.u64=0;ctx.r4.u64=boxes[0];ctx.r5.u64=scratch;ctx.r6.u64=0;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(descriptor+12));
        valid=base[boxes[0]+4]==value && base[boxes[1]+4]==0 && valid;
      }
      for(auto box:boxes){ctx.r3.u64=box;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(box)+44));valid=ctx.r3.u32==descriptor && valid;}
    }
    for(auto box:boxes)if(box){ctx.r3.u64=box;ctx.r4.u64=1;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(box)));}
  }
  if(scratch){ctx.r3.u64=scratch;ctx.r4.u64=4;__imp__sub_82655058(ctx,base);}ReleaseNativeFloatSchema(ctx,base,schema);
  if(!valid){error="Native Boolean type/storage/write/isolation validation failed";return false;}error.clear();return true;
}
std::span<const NativePropertyDeclaration> NativeNavigationDeclarations(){
  static const NativePropertyDeclaration declarations[]={
    {"PathData HPD Filename",NativePropertyShape::String},
    {"PathData Pitch Value"},
    {"PathData Bounding Box Max",NativePropertyShape::Point3},
    {"PathData Bounding Box Min",NativePropertyShape::Point3},
    {"PathData Radius",NativePropertyShape::Integer},
    {"PathData Height",NativePropertyShape::Integer},
    {"PathData Movement Type",NativePropertyShape::Integer}};
  return declarations;
}
bool ProbeNativeNavigationSchema(PPCContext& ctx,uint8_t* base,std::string& error){
  // Exercise the seven navigation field shapes without pretending the source
  // component lifecycle/defaults or live AI integration have been implemented.
  const auto declarations=NativeNavigationDeclarations();
  const char* types[]={"String","Float","Point3","Point3","Integer","Integer","Integer"};
  Context restore(ctx);NativeFloatSchema schema;
  if(!BuildNativePropertySchema(ctx,base,declarations,schema,error))return false;
  const auto scratch=Allocate(ctx,base,16);bool valid=scratch!=0;
  std::string failure;
  for(size_t i=0;i<schema.descriptors.size() && valid;++i){
    const auto descriptor=schema.descriptors[i],table=PPC_LOAD_U32(descriptor);
    ctx.r3.u64=descriptor;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+20));
    const std::string actual(reinterpret_cast<const char*>(base+PPC_LOAD_U32(ctx.r3.u32)));
    if(actual!=types[i]){valid=false;failure="Expected "+std::string(types[i])+", got "+actual;break;}
    ctx.r3.u64=descriptor;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+24));valid=ctx.r3.u32==0;
    std::array<uint32_t,2> boxes{};
    for(auto& box:boxes){ctx.r3.u64=descriptor;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+4));box=ctx.r3.u32;valid=valid && box!=0;}
    if(valid){
      const auto shape=declarations[i].shape;const unsigned words=shape==NativePropertyShape::Point3?3:1;
      std::memset(base+scratch,0,16);
      // The stock String default reader assigns to an initialized string,
      // rather than writing an unowned four-byte output buffer.
      if(shape==NativePropertyShape::String)PPC_STORE_U32(scratch,PPC_LOAD_U32(0x8203D278));
      ctx.r3.u64=descriptor;ctx.r4.u64=scratch;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+28));
      for(auto box:boxes){
        valid=valid && std::memcmp(base+box+4,base+scratch,words*4)==0;
        ctx.r3.u64=box;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(box)+44));valid=valid && ctx.r3.u32==descriptor;
      }
      std::array<uint32_t,3> defaults{};for(unsigned k=0;k<words;++k)defaults[k]=PPC_LOAD_U32(scratch+k*4);
      const uint32_t values[]={shape==NativePropertyShape::String?PPC_LOAD_U32(descriptor+4):shape==NativePropertyShape::Integer?0x80000007u:std::bit_cast<uint32_t>(1.25f),std::bit_cast<uint32_t>(-2.5f),std::bit_cast<uint32_t>(7.f)};
      // Retail82504410 skips the immortal empty/static strings and increments
      // the dynamic string's reference counter at data-8.
      const bool counted=shape==NativePropertyShape::String && values[0]!=PPC_LOAD_U32(0x8203D278) && !(values[0]>0x80000000u && values[0]<0xA0000000u);
      const auto references=counted?PPC_LOAD_U32(values[0]-8):0;
      for(unsigned k=0;k<words;++k)PPC_STORE_U32(scratch+k*4,values[k]);
      for(unsigned repeat=0;repeat<32;++repeat){ctx.r3.u64=0;ctx.r4.u64=boxes[0];ctx.r5.u64=scratch;ctx.r6.u64=0;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(descriptor+12));}
      if(counted)valid=valid && PPC_LOAD_U32(values[0]-8)==references+1;
      for(unsigned k=0;k<words;++k)valid=valid && PPC_LOAD_U32(boxes[0]+4+k*4)==values[k] && PPC_LOAD_U32(boxes[1]+4+k*4)==defaults[k];
      // Restore through the setter, including native release of string values.
      for(unsigned k=0;k<words;++k)PPC_STORE_U32(scratch+k*4,defaults[k]);
      ctx.r3.u64=0;ctx.r4.u64=boxes[0];ctx.r5.u64=scratch;ctx.r6.u64=0;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(descriptor+12));
      if(counted)valid=valid && PPC_LOAD_U32(values[0]-8)==references;
      valid=valid && std::memcmp(base+boxes[0]+4,base+boxes[1]+4,words*4)==0;
    }
    for(auto box:boxes)if(box){ctx.r3.u64=box;ctx.r4.u64=1;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(box)));}
    if(!valid && failure.empty())failure="Native scalar defaults, identity, write/isolation or restore failed for "+declarations[i].name;
  }
  if(scratch){ctx.r3.u64=scratch;ctx.r4.u64=16;__imp__sub_82655058(ctx,base);}
  ReleaseNativeFloatSchema(ctx,base,schema);
  if(!valid){error=failure.empty()?"Native navigation scalar scratch allocation failed":failure;return false;}
  error.clear();return true;
}
bool ProbeNativeArraySchema(PPCContext& ctx,uint8_t* base,std::string& error){
  const NativePropertyDeclaration declarations[]={{"em1_probe_float_array",NativePropertyShape::FloatArray},{"em1_probe_integer_array",NativePropertyShape::IntegerArray},{"em1_probe_string_array",NativePropertyShape::StringArray}};
  Context restore(ctx);NativeFloatSchema schema;if(!BuildNativePropertySchema(ctx,base,declarations,schema,error))return false;
  const auto scratch=Allocate(ctx,base,16);bool valid=scratch!=0;const char* types[]={"Float","Integer","String"};
  for(size_t i=0;valid && i<schema.descriptors.size();++i){const auto descriptor=schema.descriptors[i],table=PPC_LOAD_U32(descriptor);
    ctx.r3.u64=descriptor;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+24));valid=valid && ctx.r3.u32==1;
    ctx.r3.u64=descriptor;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+20));valid=valid && std::strcmp(reinterpret_cast<const char*>(base+PPC_LOAD_U32(ctx.r3.u32)),types[i])==0;
    ctx.r3.u64=descriptor;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+4));const auto box=ctx.r3.u32;
    if(!box){valid=false;break;}
    const auto vtable=PPC_LOAD_U32(box);ctx.r3.u64=box;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(vtable+44));valid=valid && ctx.r3.u32==descriptor;
    ctx.r3.u64=box;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(vtable+8));valid=valid && ctx.r3.u32==0;
    ctx.r3.u64=box;ctx.r4.u64=3;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(vtable+12));
    ctx.r3.u64=box;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(vtable+8));valid=valid && ctx.r3.u32==3;
    for(uint32_t index=0;index<3;++index){
      const auto expected=i==0?std::bit_cast<uint32_t>(float(index)+1.25f):i==1?uint32_t(-7+int(index)):PPC_LOAD_U32(descriptor+4);
      PPC_STORE_U32(scratch,expected);ctx.r3.u64=0;ctx.r4.u64=box;ctx.r5.u64=scratch;ctx.r6.u64=index;
      PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(descriptor+20));valid=valid && PPC_LOAD_U32(PPC_LOAD_U32(box+4)+index*4)==expected;
    }
    ctx.r3.u64=box;ctx.r4.u64=0;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(vtable+12));
    ctx.r3.u64=box;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(vtable+8));valid=valid && ctx.r3.u32==0;
    ctx.r3.u64=box;ctx.r4.u64=1;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(vtable));
  }
  if(scratch){ctx.r3.u64=scratch;ctx.r4.u64=16;__imp__sub_82655058(ctx,base);}
  ReleaseNativeFloatSchema(ctx,base,schema);
  if(!valid){error="Native float/integer/string array storage or element parity failed";return false;}
  error.clear();return true;
}
bool ProbeNativeFloatSchema(PPCContext& ctx,uint8_t* base,std::string& error){
  // Synthetic metadata fixture, not a replacement GSA/component/level.
  const FloatPropertyDeclaration declarations[]={{"em1_probe_float_positive",1.25f},{"em1_probe_float_negative",-2.5f},{"em1_probe_float_zero",-0.0f}};
  Context restore(ctx);NativeFloatSchema schema;
  if(!BuildNativeFloatSchema(ctx,base,declarations,schema,error))return false;
  const auto owned=schema.storage;std::string rejected;
  bool invalid_rejected=!BuildNativeFloatSchema(ctx,base,declarations,schema,rejected) && schema.storage==owned && schema.descriptors.size()==3;
  NativeFloatSchema invalid;const FloatPropertyDeclaration duplicate[]={{"duplicate",1},{"duplicate",2}};
  invalid_rejected=invalid_rejected && !BuildNativeFloatSchema(ctx,base,duplicate,invalid,rejected) && !invalid.storage && invalid.descriptors.empty();
  const FloatPropertyDeclaration nonfinite[]={{"invalid",std::bit_cast<float>(0x7FC00000u)}};
  invalid_rejected=invalid_rejected && !BuildNativeFloatSchema(ctx,base,nonfinite,invalid,rejected) && !invalid.storage && invalid.descriptors.empty();
  const auto scratch=Allocate(ctx,base,16);bool valid=scratch!=0 && invalid_rejected;
  if(valid)for(size_t i=0;i<schema.descriptors.size();++i){const auto descriptor=schema.descriptors[i];const auto table=PPC_LOAD_U32(descriptor);const auto expected=std::bit_cast<uint32_t>(declarations[i].default_value);
    ctx.r3.u64=descriptor;ctx.r4.u64=scratch;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+28));valid=valid && PPC_LOAD_U32(scratch)==expected;
    ctx.r3.u64=descriptor;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+20));
    const auto type=PPC_LOAD_U32(ctx.r3.u32);valid=valid && std::memcmp(base+type,"Float",6)==0;
    ctx.r3.u64=descriptor;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+4));const auto box=ctx.r3.u32;
    if(!box){valid=false;break;}
    valid=valid && PPC_LOAD_U32(box+4)==expected;
    ctx.r3.u64=box;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(box)+44));valid=valid && ctx.r3.u32==descriptor;
    ctx.r3.u64=0;ctx.r4.u64=box;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(descriptor+16));valid=valid && ctx.r3.u32==box+4;
    PPC_STORE_U32(scratch,std::bit_cast<uint32_t>(float(i)+17.5f));ctx.r3.u64=0;ctx.r4.u64=box;ctx.r5.u64=scratch;
    PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(descriptor+12));valid=valid && PPC_LOAD_U32(box+4)==PPC_LOAD_U32(scratch);
    ctx.r3.u64=box;ctx.r4.u64=1;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(box)));
  }
  if(scratch){ctx.r3.u64=scratch;ctx.r4.u64=16;__imp__sub_82655058(ctx,base);}
  ReleaseNativeFloatSchema(ctx,base,schema);
  valid=valid && !schema.storage && !schema.storage_size && schema.descriptors.empty();
  if(valid){
    valid=BuildNativeAmmoSchema(ctx,base,schema,error);
    const auto properties=LegacyAmmoProperties();valid=valid && schema.descriptors.size()==properties.size();
    const auto value=Allocate(ctx,base,4);valid=valid && value!=0;
    if(value)for(size_t i=0;i<schema.descriptors.size();++i){const auto descriptor=schema.descriptors[i],table=PPC_LOAD_U32(descriptor);
      valid=valid && std::strcmp(reinterpret_cast<const char*>(base+PPC_LOAD_U32(descriptor+4)),properties[i].name.data())==0;
      ctx.r3.u64=descriptor;ctx.r4.u64=value;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+28));
      valid=valid && PPC_LOAD_U32(value)==std::bit_cast<uint32_t>(properties[i].default_value);
      ctx.r3.u64=descriptor;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+4));const auto box=ctx.r3.u32;
      if(!box){valid=false;break;}valid=valid && PPC_LOAD_U32(box+4)==PPC_LOAD_U32(value);
      ctx.r3.u64=box;ctx.r4.u64=1;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(box)));
    }
    if(value){ctx.r3.u64=value;ctx.r4.u64=4;__imp__sub_82655058(ctx,base);}
    ReleaseNativeFloatSchema(ctx,base,schema);
  }
  if(!valid){error="Native float default/boxed access parity failed";return false;}
  error.clear();return true;
}
}
