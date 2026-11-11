#include "em1_havok_collision.h"
#include "em1_havok_pack.h"
#include "em1_triangle_key_oracle.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>
namespace {
using namespace em2::em1;
[[noreturn]] void Fail(const char* text){throw std::runtime_error(text);}
void Need(bool value,const char* text){if(!value)Fail(text);}
uint8_t B8(std::span<const uint8_t> b,size_t p){Need(p<b.size(),"Havok8 range");return b[p];}
uint16_t U16(std::span<const uint8_t> b,size_t p){Need(p<=b.size()&&b.size()-p>=2,"Havok16 range");return uint16_t(b[p]<<8|b[p+1]);}
uint32_t U32(std::span<const uint8_t> b,size_t p){Need(p<=b.size()&&b.size()-p>=4,"Havok32 range");return uint32_t(b[p])<<24|uint32_t(b[p+1])<<16|uint32_t(b[p+2])<<8|b[p+3];}
float F32(std::span<const uint8_t> b,size_t p){const float f=std::bit_cast<float>(U32(b,p));Need(std::isfinite(f),"Nonfinite authored collision value");return f;}
void W16(std::vector<uint8_t>& b,size_t p,uint16_t v){Need(p+2<=b.size(),"Native16 write range");b[p]=uint8_t(v>>8);b[p+1]=uint8_t(v);}
void W32(std::vector<uint8_t>& b,size_t p,uint32_t v){Need(p+4<=b.size(),"Native32 write range");for(unsigned i=0;i<4;++i)b[p+i]=uint8_t(v>>(24-i*8));}
void WF(std::vector<uint8_t>& b,size_t p,float v){W32(b,p,std::bit_cast<uint32_t>(v));}
using em2::em1::hkx::Location;
using em2::em1::hkx::Section;
using em2::em1::hkx::Pack;
struct Builder {
 std::vector<uint8_t> data;
 std::vector<std::array<uint32_t,2>> local;
 std::vector<std::array<uint32_t,3>> global,virtuals;
 uint32_t Allocate(uint64_t size){Need(size<=64*1024*1024&&data.size()+size+15<=64*1024*1024,"Native collision pack size limit");data.resize((data.size()+15)&~size_t(15));const auto p=uint32_t(data.size());data.resize(data.size()+size);return p;}
 void Pointer(uint32_t from,uint32_t to,bool object=false){if(object)global.push_back({from,2,to});else local.push_back({from,to});}
 void Array(uint32_t field,uint32_t target,uint32_t count){if(count)Pointer(field,target);W32(data,field+4,count);W32(data,field+8,0x80000000u|count);}
 void Empty(uint32_t field){Array(field,0,0);}
};
void Identity(std::vector<uint8_t>& b,uint32_t part){WF(b,part+0x5c,1);for(unsigned k=0;k<4;++k)WF(b,part+0x60+k*4,1);}
std::vector<uint8_t> NativeShape(const AuthoredCollisionBody& body,bool allow_acceleration=true){
 Need(!body.parts.empty()&&body.parts.size()<=1024,"Native mesh subpart count");Builder w;const auto shape=w.Allocate(0x110);Need(shape==0,"Native root offset");
 W32(w.data,8,body.shape_user_data);w.data[0x14]=body.disable_welding;w.data[0x15]=1;Identity(w.data,0x20);
 W32(w.data,0xb4,body.key_bits);w.data[0xdc]=body.welding_type;W32(w.data,0xe0,body.default_filter);WF(w.data,0xe8,body.radius);
 const auto parts=w.Allocate(body.parts.size()*0x70),storages=w.Allocate(body.parts.size()*4);
 w.Array(0xb8,parts,uint32_t(body.parts.size()));w.Empty(0xc4);w.Array(0xf0,storages,uint32_t(body.parts.size()));w.Empty(0xfc);
 uint32_t weld=0;if(!body.welding.empty()){weld=w.Allocate(body.welding.size()*2);for(size_t i=0;i<body.welding.size();++i)W16(w.data,weld+i*2,body.welding[i]);}w.Array(0xd0,weld,uint32_t(body.welding.size()));
 std::array<float,3> minimum={INFINITY,INFINITY,INFINITY},maximum={-INFINITY,-INFINITY,-INFINITY};uint32_t total=0;
 for(size_t i=0;i<body.parts.size();++i){const auto& p=body.parts[i];const uint32_t part=parts+uint32_t(i)*0x70,storage=w.Allocate(0x68);w.Pointer(storages+uint32_t(i)*4,storage,true);w.virtuals.push_back({storage,0,1});
  for(uint32_t field:{8u,0x14u,0x20u,0x2cu,0x38u,0x44u,0x50u,0x5cu})w.Empty(storage+field);
  Need(p.triangle_filters.size()==p.triangles.size(),"Incomplete authored triangle filters");
  const auto vertices=w.Allocate(p.vertices.size()*16);for(size_t j=0;j<p.vertices.size();++j)for(unsigned k=0;k<3;++k){WF(w.data,vertices+j*16+k*4,p.vertices[j][k]);minimum[k]=std::min(minimum[k],p.vertices[j][k]);maximum[k]=std::max(maximum[k],p.vertices[j][k]);}
  const auto indices=w.Allocate(p.triangles.size()*12);for(size_t j=0;j<p.triangles.size();++j)for(unsigned k=0;k<3;++k){Need(p.triangles[j][k]<p.vertices.size(),"Authored triangle vertex range");W32(w.data,indices+j*12+k*4,p.triangles[j][k]);}
  w.Array(storage+8,vertices,uint32_t(p.vertices.size()));w.Array(storage+0x2c,indices,uint32_t(p.triangles.size()*3));
  std::map<uint32_t,uint16_t> filters;std::vector<uint32_t> order;for(auto f:p.triangle_filters)if(!filters.contains(f)){Need(filters.size()<8191,"Native material count limit");filters[f]=uint16_t(filters.size());order.push_back(f);}
  const uint32_t width=filters.size()<=256?1:2,material_indices=w.Allocate(p.triangles.size()*width),materials=w.Allocate(filters.size()*12);
  for(size_t j=0;j<p.triangle_filters.size();++j){const auto index=filters.at(p.triangle_filters[j]);if(width==1)w.data[material_indices+j]=uint8_t(index);else W16(w.data,material_indices+j*2,index);}
  for(size_t j=0;j<order.size();++j){W32(w.data,materials+j*12,order[j]);W16(w.data,materials+j*12+4,uint16_t(std::bit_cast<uint32_t>(body.restitution)>>16));W16(w.data,materials+j*12+6,uint16_t(std::bit_cast<uint32_t>(body.friction)>>16));W32(w.data,materials+j*12+8,p.user_data);}
  w.Array(storage+(width==1?0x38:0x5c),material_indices,uint32_t(p.triangles.size()));w.Array(storage+0x44,materials,uint32_t(filters.size()));
  // Native2011 Subpart packs material count/type; HK7 stores them separately.
  W16(w.data,part,uint16_t((filters.size()<<3)|(width<<1)));W16(w.data,part+4,12);W16(w.data,part+6,uint16_t(width));w.Pointer(part+8,material_indices);w.Pointer(part+0xc,materials);W32(w.data,part+0x10,p.user_data);
  W32(w.data,part+0x14,uint32_t(p.triangles.size()));w.Pointer(part+0x18,vertices);W32(w.data,part+0x1c,uint32_t(p.vertices.size()));w.Pointer(part+0x20,indices);W16(w.data,part+0x24,16);W32(w.data,part+0x28,p.welding_offset);W16(w.data,part+0x2c,12);w.data[part+0x2e]=3;Identity(w.data,part);total+=uint32_t(p.triangles.size());
 }
 // Native getChildShape uses the inline triangle subpart when there is only
 // one part (82457A88), rather than reading the subpart array.
 if(body.parts.size()==1){
  std::copy_n(w.data.begin()+parts,0x70,w.data.begin()+0x20);
  for(const uint32_t field:{8u,0xcu,0x18u,0x20u}){
   const auto found=std::find_if(w.local.begin(),w.local.end(),[&](const auto& p){return p[0]==parts+field;});
   Need(found!=w.local.end(),"Missing inline triangle subpart binding");const auto target=(*found)[1];w.Pointer(0x20+field,target);
  }
 }
 W32(w.data,0xe4,total);for(unsigned k=0;k<3;++k){WF(w.data,0x90+k*4,(maximum[k]-minimum[k])*.5f+body.radius);WF(w.data,0xa0+k*4,(maximum[k]+minimum[k])*.5f);}
 uint32_t root=0,root_class=0;
 if(body.accelerated && allow_acceleration){
  Need(!body.original_mopp_code.empty() && body.key_bits>1 && body.key_bits<=31,"Missing authored acceleration program/keyspace");
  root=w.Allocate(0x40);const auto code=w.Allocate(0x30),program=w.Allocate(body.original_mopp_code.size());
  root_class=2;w.virtuals.push_back({root,0,2});w.virtuals.push_back({code,0,3});
  W32(w.data,root+8,body.original_mopp_user_data);w.data[root+0x10]=body.original_mopp_type;
  w.Pointer(root+0x14,code,true);w.Pointer(root+0x34,shape,true);
  for(unsigned i=0;i<4;++i)WF(w.data,code+0x10+i*4,body.original_mopp_info[i]);
  std::copy(body.original_mopp_code.begin(),body.original_mopp_code.end(),w.data.begin()+program);
  w.Array(code+0x20,program,uint32_t(body.original_mopp_code.size()));w.data[code+0x2c]=body.original_mopp_build_type;
 }
 w.Allocate(0);const uint32_t local_start=uint32_t(w.data.size());auto emit=[&](auto records){for(const auto& record:records)for(uint32_t x:record){const auto p=w.data.size();w.data.resize(p+4);W32(w.data,p,x);}while(w.data.size()%16)w.data.push_back(0xff);};
 emit(w.local);const uint32_t global_start=uint32_t(w.data.size());emit(w.global);const uint32_t virtual_start=uint32_t(w.data.size());
 std::vector<uint8_t> names;std::array<uint32_t,4> name_offsets{};
 // Signatures verified from native Xbox2011 dbc_courtyard_01a_scpt.pak,
 // DBC_courtyard_01a_hallwayThinner_inert_01a.hkx (SHA256 c88b1d6b...26ae43).
 const std::array<std::pair<std::string_view,uint32_t>,4> classes={{{"hkpStorageExtendedMeshShape",0xd424d9b6},{"hkpStorageExtendedMeshShapeMeshSubpartStorage",0x5aad4de6},{"hkpMoppBvTreeShape",0x802b729d},{"hkpMoppCode",0x924c2661}}};
 for(size_t i=0;i<classes.size();++i){const auto p=names.size();names.resize(p+5);W32(names,p,classes[i].second);names[p+4]=9;name_offsets[i]=uint32_t(names.size());names.insert(names.end(),classes[i].first.begin(),classes[i].first.end());names.push_back(0);}while(names.size()%16)names.push_back(0xff);
 w.virtuals.push_back({0,0,0});for(auto& record:w.virtuals)record[2]=name_offsets[record[2]];emit(w.virtuals);const uint32_t data_end=uint32_t(w.data.size());
 const uint32_t name_start=64+3*48,type_start=name_start+uint32_t(names.size()),data_start=type_start;
 std::vector<uint8_t> result(data_start+data_end);W32(result,0,0x57e0e057);W32(result,4,0x10c0c010);W32(result,12,9);result[16]=4;result[19]=1;W32(result,20,3);W32(result,24,2);W32(result,28,root);W32(result,32,0);W32(result,36,name_offsets[root_class]);const char version[]="hk_2011.2.0-r1";std::memcpy(result.data()+40,version,sizeof(version));result[55]=0xff;W32(result,60,UINT32_MAX);
 auto section=[&](unsigned index,const char* name,std::array<uint32_t,7> offsets){const uint32_t p=64+index*48;std::memcpy(result.data()+p,name,std::strlen(name));for(unsigned i=0;i<7;++i)W32(result,p+20+i*4,offsets[i]);};
 section(0,"__classnames__",{name_start,uint32_t(names.size()),uint32_t(names.size()),uint32_t(names.size()),uint32_t(names.size()),uint32_t(names.size()),uint32_t(names.size())});section(1,"__types__",{type_start,0,0,0,0,0,0});section(2,"__data__",{data_start,local_start,global_start,virtual_start,data_end,data_end,data_end});
 std::copy(names.begin(),names.end(),result.begin()+name_start);std::copy(w.data.begin(),w.data.end(),result.begin()+data_start);return result;
}
} // namespace
bool em2::em1::IsRigidCollisionPose(const std::array<float,16>& a,bool havok_padding){
 if(!std::all_of(a.begin(),a.end(),[](float x){return std::isfinite(x);}))return false;
 for(unsigned c=0;c<3;++c)for(unsigned d=0;d<3;++d){double dot=0;for(unsigned r=0;r<3;++r)dot+=double(a[c*4+r])*a[d*4+r];if(std::abs(dot-(c==d?1.:0.))>0.0001)return false;}
 const double det=double(a[0])*(a[5]*a[10]-a[6]*a[9])-double(a[4])*(a[1]*a[10]-a[2]*a[9])+double(a[8])*(a[1]*a[6]-a[2]*a[5]);
 return det>0.9999&&(havok_padding || (std::abs(a[3])<0.0001&&std::abs(a[7])<0.0001&&std::abs(a[11])<0.0001));
}
bool em2::em1::ReconstructAuthoredStaticCollision(std::span<const uint8_t> source,AuthoredStaticCollision& result,std::string& error){
 result={};error.clear();try{
  Pack file(source,true);
  for(const auto& [key,name]:file.classes){if(name!="hkpRigidBody")continue;Location location{uint32_t(key>>32),uint32_t(key)};const auto section=location.section,body=location.offset;auto data=file.Data(section);auto pointer=[&](uint32_t p,bool optional=false){const auto q=file.Pointer({section,p},optional);Need(q.section==section||q.section==UINT32_MAX,"Cross-section authored physics object unsupported");return q.offset;};
   Need(body+0x220<=data.size()&&data[body+0xe8]==5,"Only authored fixed Havok7 bodies supported");AuthoredCollisionBody out;out.source_offset=body;out.motion_type=5;out.filter=U32(data,body+0x2c);out.user_data=U32(data,body+0xc);out.quality_type=data[body+0x2a];std::copy_n(data.begin()+body+0x8c,out.source_material.size(),out.source_material.begin());out.friction=F32(data,body+0x90);out.restitution=F32(data,body+0x94);Need(out.friction>=0&&out.restitution>=0,"Negative authored material coefficient");
   out.name=file.String(file.Pointer({section,body+0x74}));for(unsigned i=0;i<16;++i)out.transform[i]=F32(data,body+0xf0+i*4);
   Need(IsRigidCollisionPose(out.transform,true),"Invalid authored rigid-body rotation");
   const uint32_t property_count=U32(data,body+0x7c);Need(property_count<=4096,"Authored body property count");if(property_count){const auto base=pointer(body+0x78);for(uint32_t i=0;i<property_count;++i)out.properties.push_back({U32(data,base+i*16),U32(data,base+i*16+4),U32(data,base+i*16+8),U32(data,base+i*16+12)});}
   const auto mopp=pointer(body+0x10);Need(file.Class({section,mopp})=="hkpMoppBvTreeShape","Unsupported authored static root shape");const auto code=pointer(mopp+0x14),mesh=pointer(mopp+0x34);Need(file.Class({section,code})=="hkpMoppCode"&&file.Class({section,mesh})=="hkpStorageExtendedMeshShape","Unsupported authored MOPP child");
   out.original_mopp_user_data=U32(data,mopp+8);out.original_mopp_type=B8(data,mopp+0x10);out.original_mopp_build_type=B8(data,code+0x2c);
   for(unsigned i=0;i<4;++i)out.original_mopp_info[i]=F32(data,code+0x10+i*4);const auto code_count=U32(data,code+0x24),code_at=pointer(code+0x20);Need(code_count<=16*1024*1024&&uint64_t(code_at)+code_count<=data.size(),"Authored MOPP range");out.original_mopp_code.assign(data.begin()+code_at,data.begin()+code_at+code_count);
   Need(uint64_t(mesh)+0x108<=data.size(),"Authored mesh object range");out.shape_user_data=U32(data,mesh+8);out.disable_welding=B8(data,mesh+0x14)!=0;out.welding_type=B8(data,mesh+0xd8);out.radius=F32(data,mesh+0xe4);out.key_bits=U32(data,mesh+0xb0);Need(out.radius>=0&&out.key_bits>0&&out.key_bits<=31,"Authored radius/key bits");
   out.default_filter=U32(data,mesh+0xdc);const uint32_t default_filter=out.default_filter,count=U32(data,mesh+0xb8);Need(count&&count<=1024&&count==U32(data,mesh+0xf4),"Authored mesh subpart/storage mismatch");const auto parts=pointer(mesh+0xb4),storages=pointer(mesh+0xf0);std::array<float,3> scale;for(unsigned i=0;i<3;++i){scale[i]=F32(data,mesh+0x80+i*4);Need(scale[i]>0,"Invalid authored mesh scale");}
   const auto wc=U32(data,mesh+0xd0);Need(wc<=1000000,"Authored welding count");if(wc){const auto wa=pointer(mesh+0xcc);for(uint32_t i=0;i<wc;++i)out.welding.push_back(U16(data,wa+i*2));}
   for(uint32_t i=0;i<count;++i){const auto part=parts+i*0x60,storage=pointer(storages+i*4);Need(file.Class({section,storage})=="hkpStorageExtendedMeshShapeMeshSubpartStorage","Unexpected authored mesh storage");Need(uint64_t(part)+0x60<=data.size(),"Authored subpart range");
    Need(std::all_of(data.begin()+part+0x24,data.begin()+part+0x44,[](auto v){return v==0;})&&std::all_of(data.begin()+part+0x50,data.begin()+part+0x60,[](auto v){return v==0;}),"Transformed/extruded authored subparts unsupported");Need(data[part+0x49]==0,"Alternating authored triangle winding unsupported");AuthoredCollisionPart p;p.user_data=U32(data,part+0x10);p.welding_offset=U32(data,part+0x4c);const auto nv=U32(data,storage+0xc),nt=U32(data,part+0x14);Need(nv&&nv<=1000000&&nt&&nt<=1000000&&nv==U32(data,part+0x20),"Authored geometry counts");const auto va=pointer(storage+8);for(uint32_t j=0;j<nv;++j){std::array<float,3> v;for(unsigned k=0;k<3;++k){v[k]=F32(data,va+j*16+k*4)*scale[k];Need(std::isfinite(v[k]),"Authored scaled vertex overflow");}p.vertices.push_back(v);}
    p.source_index_kind=data[part+0x48];p.source_index_stride=U32(data,part+0x44);uint32_t width=0,field=0;switch(p.source_index_kind){case 1:width=1;field=0x14;break;case 2:width=2;field=0x20;break;case 3:width=4;field=0x2c;break;default:Fail("Unsupported authored triangle index width");}
    const auto ia=pointer(storage+field),ic=U32(data,storage+field+4);Need(p.source_index_stride>=3*width&&p.source_index_stride<=64&&uint64_t(nt)*p.source_index_stride<=uint64_t(ic)*width,"Authored padded index range");for(uint32_t j=0;j<nt;++j){std::array<uint32_t,3> triangle;for(unsigned k=0;k<3;++k){const auto at=ia+j*p.source_index_stride+k*width;triangle[k]=width==1?B8(data,at):width==2?U16(data,at):U32(data,at);Need(triangle[k]<nv,"Authored triangle vertex index");}p.triangles.push_back(triangle);}
    auto mi=file.Pointer({section,part+4},true);if(mi.section==UINT32_MAX)p.triangle_filters.assign(nt,default_filter);else{Need(mi.section==section,"Cross-section material indices");const uint32_t material_base=pointer(part+0xc),material_count=U16(data,part+0xa),index_stride=U16(data,part+8),index_width=data[part+1];const int32_t material_stride=int16_t(U16(data,part+2));Need((index_width==1||index_width==2)&&index_stride>=index_width&&index_stride<=64&&material_stride>=4&&material_stride<=256&&material_count,"Unsupported authored triangle materials");for(uint32_t j=0;j<nt;++j){const auto at=mi.offset+j*index_stride,index=index_width==1?uint32_t(B8(data,at)):uint32_t(U16(data,at));Need(index<material_count,"Authored triangle material index");p.triangle_filters.push_back(U32(data,material_base+index*material_stride));}}
    Need(out.welding.empty()||uint64_t(p.welding_offset)+nt<=out.welding.size(),"Authored welding offset range");result.vertices+=nv;result.triangles+=nt;out.parts.push_back(std::move(p));
   }
   // Keep body filtering distinct from per-triangle default filtering.
   out.native_shape_pack=NativeShape(out);Pack verify(out.native_shape_pack,false);Need(verify.Class({2,0})=="hkpStorageExtendedMeshShape","Native collision root class");result.bodies.push_back(std::move(out));
  }
  Need(!result.bodies.empty(),"No authored fixed bodies in resource");return true;
 }catch(const std::exception& e){result={};error=e.what();return false;}
}
bool em2::em1::BuildUnacceleratedCollisionPack(const AuthoredCollisionBody& body,std::vector<uint8_t>& output,std::string& error){
 try{auto pack=NativeShape(body,false);output=std::move(pack);error.clear();return true;}
 catch(const std::exception& e){error=e.what();return false;}
}
bool em2::em1::AccelerateAuthoredStaticCollision(AuthoredStaticCollision& source,std::string& error){
 try{
  Need(!source.bodies.empty(),"No collision bodies to accelerate");auto candidate=source;
  for(auto& body:candidate.bodies){
   body.accelerated=true;body.native_shape_pack=NativeShape(body);
   Pack verify(body.native_shape_pack,false);const auto root=U32(body.native_shape_pack,28);
   Need(verify.Class({2,root})=="hkpMoppBvTreeShape" && verify.Pointer({2,root+0x34}).offset==0,"Native acceleration graph differs");
   const auto data=verify.Data(2);const auto code=verify.Pointer({2,root+0x14}).offset,program=verify.Pointer({2,code+0x20}).offset;
   Need(U32(data,code+0x24)==body.original_mopp_code.size() && std::equal(body.original_mopp_code.begin(),body.original_mopp_code.end(),data.begin()+program),"Authored acceleration program changed");
   for(unsigned i=0;i<4;++i)Need(U32(data,code+0x10+i*4)==std::bit_cast<uint32_t>(body.original_mopp_info[i]),"Authored acceleration quantization changed");
  }
  candidate.accelerated=true;source=std::move(candidate);error.clear();return true;
 }catch(const std::exception& e){error=e.what();return false;}
}
bool em2::em1::ScaleAuthoredStaticCollision(AuthoredStaticCollision& source,float scale,std::string& error){
 try{
  Need(std::isfinite(scale)&&scale>0&&scale<=10000,"Unsupported collision placement scale");
  if(scale==1){error.clear();return true;}
  Need(!source.accelerated,"Scale authored collision before acceleration; quantization scaling is not audited");
  for(auto& body:source.bodies){
   for(auto& part:body.parts)for(auto& vertex:part.vertices)for(auto& value:vertex){value*=scale;Need(std::isfinite(value),"Collision scale overflow");}
   for(unsigned i=12;i<15;++i){body.transform[i]*=scale;Need(std::isfinite(body.transform[i]),"Collision translation scale overflow");}
   body.radius*=scale;Need(std::isfinite(body.radius),"Collision margin scale overflow");body.native_shape_pack=NativeShape(body);
  }
  error.clear();return true;
 }catch(const std::exception& e){error=e.what();return false;}
}
bool em2::em1::TestEm1HavokCollision(std::string& error){
 error.clear();try{
 if(!TestTriangleKeyOracle(error))return false;
 std::array<float,16> original_pose={0.9010784029960632f,0,0.43365615606307983f,-0.f,
   -0.f,1,0,-0.43365615606307983f,-0.43365615606307983f,-0.f,0.9010784029960632f,0,
   6.165051460266113f,1.1715270280838013f,-76.99374389648438f,1};
 Need(IsRigidCollisionPose(original_pose,true) && !IsRigidCollisionPose(original_pose),"Original Havok SIMD padding confused with affine placement");
 auto rigid=original_pose;rigid[3]=rigid[7]=rigid[11]=0;
 Need(IsRigidCollisionPose(rigid) && IsRigidCollisionPose(rigid,true),"Rigid pose rejected");
 auto malformed=rigid;malformed[0]*=2;Need(!IsRigidCollisionPose(malformed,true),"Scaled body accepted as rigid");
 malformed=rigid;malformed[4]=.25f;Need(!IsRigidCollisionPose(malformed,true),"Sheared body accepted as rigid");
 malformed=rigid;for(unsigned i=0;i<3;++i)malformed[i]=-malformed[i];Need(!IsRigidCollisionPose(malformed,true),"Reflected body accepted as proper rotation");
 malformed=rigid;malformed[12]=std::numeric_limits<float>::quiet_NaN();Need(!IsRigidCollisionPose(malformed,true),"Nonfinite body translation accepted");
 AuthoredCollisionBody b;b.radius=.05f;b.key_bits=12;b.friction=.5f;b.restitution=.1f;AuthoredCollisionPart p;p.vertices={{{0,0,0}},{{1,0,0}},{{0,0,1}}};p.triangles={{{0,1,2}}};p.triangle_filters={0x12340007};b.parts.push_back(p);auto bytes=NativeShape(b);Pack native(bytes,false);Need(native.Class({2,0})=="hkpStorageExtendedMeshShape","Native root class fixture");auto d=native.Data(2);const auto part=native.Pointer({2,0xb8}).offset;Need(U32(d,part+0x14)==1&&U32(d,part+0x1c)==3&&d[part+0x2e]==3,"Native triangle schema fixture");const auto indices=native.Pointer({2,part+0x20}).offset;Need(U32(d,indices)==0&&U32(d,indices+4)==1&&U32(d,indices+8)==2,"Native triangle order fixture");const auto material=native.Pointer({2,part+0xc}).offset;Need(U32(d,material)==0x12340007,"Native filter fixture");Need(U16(d,part)==10&&U16(d,part+4)==12&&U16(d,part+6)==1,"Native packed material schema fixture");Need(U32(d,0xe4)==1&&F32(d,0xe8)==.05f,"Native radius/count fixture");Need(F32(d,0x90)==.5f+b.radius&&F32(d,0x94)==b.radius&&F32(d,0x98)==.5f+b.radius,"Native cached bounds include triangle radius");auto bad=bytes;bad[18]=1;bool rejected=false;try{Pack invalid(bad,false);}catch(...){rejected=true;}Need(rejected,"Wrong native layout accepted");
 b.accelerated=true;b.native_shape_pack={0xaa,0xbb};std::vector<uint8_t> fallback{0xcc};std::string why;
 Need(BuildUnacceleratedCollisionPack(b,fallback,why) && fallback==bytes,"Body-only fallback changed authored geometry, filters, welding, bounds or fixups");
 Need(b.accelerated && b.native_shape_pack==std::vector<uint8_t>{0xaa,0xbb},"Fallback mutated its accelerated source owner");
 b.parts[0].triangle_filters.clear();
 Need(!BuildUnacceleratedCollisionPack(b,fallback,why) && fallback==bytes,"Invalid body fallback accepted or replaced previous output");
 return true;}catch(const std::exception& e){error=e.what();return false;}
}
