#include "em1_jigsaw_havok.h"
#include "em1_jigsaw_bindings.h"
#include "em1_havok_collision.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

// Narrow local Pack/Builder helpers match the independently audited collision
// reader/writer. Kept isolated while the static collision proof is in progress.
namespace {
using namespace em2::em1;
using nif::Bytes;
[[noreturn]] void Fail(const char* text){throw std::runtime_error(text);}
void Need(bool value,const char* text){if(!value)Fail(text);}
uint8_t B8(std::span<const uint8_t> b,size_t p){Need(p<b.size(),"Havok8 range");return b[p];}
uint16_t U16(std::span<const uint8_t> b,size_t p){Need(p<=b.size()&&b.size()-p>=2,"Havok16 range");return uint16_t(b[p]<<8|b[p+1]);}
uint32_t U32(std::span<const uint8_t> b,size_t p){Need(p<=b.size()&&b.size()-p>=4,"Havok32 range");return uint32_t(b[p])<<24|uint32_t(b[p+1])<<16|uint32_t(b[p+2])<<8|b[p+3];}
float F32(std::span<const uint8_t> b,size_t p){const float f=std::bit_cast<float>(U32(b,p));Need(std::isfinite(f),"Nonfinite authored collision value");return f;}
void W16(std::vector<uint8_t>& b,size_t p,uint16_t v){Need(p+2<=b.size(),"Native16 write range");b[p]=uint8_t(v>>8);b[p+1]=uint8_t(v);}
void W32(std::vector<uint8_t>& b,size_t p,uint32_t v){Need(p+4<=b.size(),"Native32 write range");for(unsigned i=0;i<4;++i)b[p+i]=uint8_t(v>>(24-i*8));}
void WF(std::vector<uint8_t>& b,size_t p,float v){W32(b,p,std::bit_cast<uint32_t>(v));}
struct Location{uint32_t section,offset;};
struct Section{uint32_t start,local,global,virtuals,exports,imports,end;};
class Pack {
public:
 std::span<const uint8_t> bytes;std::vector<Section> sections;
 std::map<uint64_t,Location> pointers;std::map<uint64_t,std::string> classes;
 static uint64_t Key(Location p){return uint64_t(p.section)<<32|p.offset;}
 std::span<const uint8_t> Data(uint32_t section)const{Need(section<sections.size(),"HKX section index");const auto& s=sections[section];return bytes.subspan(s.start,s.local);}
 std::string String(Location p)const{auto d=Data(p.section);Need(p.offset<d.size(),"HKX string range");auto end=std::find(d.begin()+p.offset,d.end(),0);Need(end!=d.end(),"Unterminated HKX string");return std::string(d.begin()+p.offset,end);}
 Location Pointer(Location p,bool optional=false)const{auto i=pointers.find(Key(p));if(i!=pointers.end())return i->second;Need(optional&&U32(Data(p.section),p.offset)==0,"Missing authored HKX fixup");return {UINT32_MAX,0};}
 std::string Class(Location p)const{auto i=classes.find(Key(p));Need(i!=classes.end(),"Missing authored object class");return i->second;}
 explicit Pack(std::span<const uint8_t> source,bool original):bytes(source){
  Need(bytes.size()>=64&&U32(bytes,0)==0x57E0E057&&U32(bytes,4)==0x10C0C010,"Not a Havok packfile");
  Need(U32(bytes,12)==(original?7u:9u)&&bytes[16]==4&&bytes[17]==0&&bytes[18]==(original?1:0)&&bytes[19]==1,"Unsupported Havok packfile ABI");
  const std::string_view version(reinterpret_cast<const char*>(bytes.data()+40),16);
  Need(version.starts_with(original?"Havok-7.0.0-r1":"hk_2011.2.0-r1"),"Unexpected Havok schema version");
  const auto count=U32(bytes,20);Need(count==3&&bytes.size()>=64+48*count,"Unsupported HKX section table");
  for(uint32_t i=0;i<count;++i){const auto p=84+i*48;Section s{U32(bytes,p),U32(bytes,p+4),U32(bytes,p+8),U32(bytes,p+12),U32(bytes,p+16),U32(bytes,p+20),U32(bytes,p+24)};
   Need(s.local<=s.global&&s.global<=s.virtuals&&s.virtuals<=s.exports&&s.exports<=s.imports&&s.imports<=s.end&&uint64_t(s.start)+s.end<=bytes.size(),"Invalid HKX section spans");sections.push_back(s);}
  auto insert=[&](Location from,Location to){Need(uint64_t(from.offset)+4<=Data(from.section).size()&&to.section<count&&to.offset<Data(to.section).size(),"Invalid HKX pointer fixup");Need(pointers.emplace(Key(from),to).second,"Duplicate HKX pointer fixup");};
  for(uint32_t n=0;n<count;++n){const auto& s=sections[n];
   for(uint32_t p=s.local;p+8<=s.global;p+=8){const auto a=U32(bytes,s.start+p);if(a==UINT32_MAX)break;insert({n,a},{n,U32(bytes,s.start+p+4)});}
   for(uint32_t p=s.global;p+12<=s.virtuals;p+=12){const auto a=U32(bytes,s.start+p);if(a==UINT32_MAX)break;insert({n,a},{U32(bytes,s.start+p+4),U32(bytes,s.start+p+8)});}
   for(uint32_t p=s.virtuals;p+12<=s.exports;p+=12){const auto a=U32(bytes,s.start+p);if(a==UINT32_MAX)break;Need(a<Data(n).size(),"HKX virtual fixup range");Need(classes.emplace(Key({n,a}),String({U32(bytes,s.start+p+4),U32(bytes,s.start+p+8)})).second,"Duplicate HKX virtual fixup");}
  }
 }
};
struct Builder {
 std::vector<uint8_t> data;
 std::vector<std::array<uint32_t,2>> local;
 std::vector<std::array<uint32_t,3>> global,virtuals;
 uint32_t Allocate(uint64_t size){Need(size<=64*1024*1024&&data.size()+size+15<=64*1024*1024,"Native collision pack size limit");data.resize((data.size()+15)&~size_t(15));const auto p=uint32_t(data.size());data.resize(data.size()+size);return p;}
 void Pointer(uint32_t from,uint32_t to,bool object=false){if(object)global.push_back({from,2,to});else local.push_back({from,to});}
 void Array(uint32_t field,uint32_t target,uint32_t count){if(count)Pointer(field,target);W32(data,field+4,count);W32(data,field+8,0x80000000u|count);}
 void Empty(uint32_t field){Array(field,0,0);}
};

void Copy(Bytes& destination,uint32_t at,std::span<const uint8_t> source) {
 Need(uint64_t(at)+source.size()<=destination.size(),"Jigsaw destination range");
 std::copy(source.begin(),source.end(),destination.begin()+at);
}
void IdentityTransform(Bytes& data,uint32_t part) {
 WF(data,part+0x5c,1);for(unsigned i=0;i<4;++i)WF(data,part+0x60+i*4,1);
}
struct Converter {
 Pack source;Builder output;JigsawAggregateInfo info;
 std::span<const uint8_t> data;
 std::map<uint32_t,uint32_t> convex;
 std::map<uint32_t,uint32_t> boxes;
 std::set<uint32_t> resolving_boxes;
 std::map<std::pair<uintptr_t,size_t>,uint32_t> copied_buffers;
 const JigsawMeshBindings* bindings=nullptr;
 explicit Converter(std::span<const uint8_t> input,bool body_shape=false,const JigsawMeshBindings* external=nullptr):source(input,true),data(source.Data(2)),bindings(external) {
  Need(U32(input,24)==2 && U32(input,28)==0 && source.Class({2,0})==(body_shape?"hkRootLevelContainer":"hkpMoppBvTreeShape"),"Unexpected authored collision root");
  Need(data.size()<=32*1024*1024,"Jigsaw source data budget");
  if(body_shape)return; // The standalone entrypoint checks its direct body/shape closure.
  for(const auto& [location,type]:source.classes)if(uint32_t(location>>32)==2)
   Need(type=="hkpMoppBvTreeShape" || type=="hkpMoppCode" || type=="hkpExtendedMeshShape" || type=="hkpConvexVerticesShape","Unsupported authored Jigsaw object class");
 }
 uint32_t Pointer(uint32_t at,bool optional=false)const {
  Location target;
  try{target=source.Pointer({2,at},optional);}
  catch(const std::exception& exception){throw std::runtime_error(std::string(exception.what())+" at Jigsaw data offset "+std::to_string(at));}
  Need(target.section==2 || target.section==UINT32_MAX,"Cross-section Jigsaw data pointer unsupported");
  return target.section==UINT32_MAX?UINT32_MAX:target.offset;
 }
 std::span<const uint8_t> Range(uint32_t at,uint64_t count)const {
  Need(at<=data.size() && count<=data.size()-at,"Jigsaw authored array range");return data.subspan(at,size_t(count));
 }
 uint32_t Blob(uint32_t source_at,uint64_t bytes) {
  const auto input=Range(source_at,bytes);const auto target=output.Allocate(bytes);Copy(output.data,target,input);return target;
 }
 uint32_t BoundBuffer(nif::View bytes){
  const auto key=std::pair{reinterpret_cast<uintptr_t>(bytes.data()),bytes.size()};
  if(auto found=copied_buffers.find(key);found!=copied_buffers.end())return found->second;
  const auto target=output.Allocate(bytes.size());Copy(output.data,target,bytes);copied_buffers.emplace(key,target);return target;
 }
 void ScalarVector(uint32_t old,uint32_t target,unsigned words) {
  for(unsigned i=0;i<words;++i){F32(data,old+i*4);W32(output.data,target+i*4,U32(data,old+i*4));}
 }
 void Array(uint32_t old,uint32_t target,uint32_t width,uint32_t maximum) {
  const auto count=U32(data,old+4);Need(count<=maximum,"Jigsaw array count budget");
  if(!count){output.Empty(target);return;}
  const auto at=Pointer(old),copied=Blob(at,uint64_t(count)*width);output.Array(target,copied,count);
 }
 void SubpartBase(uint32_t old,uint32_t target,uint32_t elements,uint32_t user) {
  const auto type=B8(data,old),kind=B8(data,old+1);
  const auto materials=U16(data,old+10);
  Need(type<=1 && kind<=2 && materials<=8191,"Unsupported Jigsaw subpart flags/material count");
  W16(output.data,target,uint16_t((materials<<3)|(kind<<1)|type));
  W16(output.data,target+4,U16(data,old+2));W16(output.data,target+6,U16(data,old+8));W32(output.data,target+16,user);
  const auto indices=Pointer(old+4,true),material=Pointer(old+12,true);
  if(indices!=UINT32_MAX){
   const uint32_t stride=U16(data,old+8);Need(kind && stride>=kind && stride<=256,"Jigsaw material-index stride");
   const auto buffer=Blob(indices,uint64_t(elements)*stride);output.Pointer(target+8,buffer);
  }
  if(material!=UINT32_MAX){
   const auto stride=int16_t(U16(data,old+2));Need(materials && stride>=4 && stride<=256,"Jigsaw material stride");
   // Generic old reflected material layouts need their own field adapter.
   Need(!source.pointers.contains(Pack::Key({2,old+16})) && U32(data,old+16)==0,"Reflected Jigsaw materials require a dedicated adapter");
   const auto buffer=Blob(material,uint64_t(materials)*uint32_t(stride));output.Pointer(target+12,buffer);
  }
 }
 uint32_t Convex(uint32_t old) {
  if(auto found=convex.find(old);found!=convex.end())return found->second;
  Need(source.Class({2,old})=="hkpConvexVerticesShape","Unsupported Jigsaw convex child class");Range(old,0x70);
  Need(Pointer(old+0x50,true)==UINT32_MAX,"Compressed convex-piece optimization requires an explicit adapter");
  Need(Pointer(old+0x60,true)==UINT32_MAX,"Jigsaw convex connectivity requires an explicit adapter");
  const auto target=output.Allocate(0x70);convex[old]=target;++info.convex_shapes;
  output.virtuals.push_back({target,0,3});W32(output.data,target+8,U32(data,old+8));
  Need(F32(data,old+0x10)>=0,"Invalid authored convex radius");W32(output.data,target+0x10,U32(data,old+0x10));
  ScalarVector(old+0x20,target+0x20,8);
  const auto groups=U32(data,old+0x44),vertices=U32(data,old+0x4c);
  Need(groups && groups<=262144 && vertices && vertices<=uint64_t(groups)*4 && uint64_t(vertices)+3>=uint64_t(groups)*4,"Invalid convex four-vector counts");
  Need(uint64_t(info.convex_vertices.size())+vertices<=1000000,"Cumulative Jigsaw convex proof budget");
  const auto rotated=Pointer(old+0x40);Range(rotated,uint64_t(groups)*48);
  for(uint32_t i=0;i<vertices;++i){std::array<float,3> vertex{};for(unsigned j=0;j<3;++j)vertex[j]=F32(data,rotated+(i/4)*48+j*16+(i%4)*4);info.convex_vertices.push_back(vertex);}
  Array(old+0x40,target+0x40,48,262144);W32(output.data,target+0x4c,vertices);
  // Old convexPiece pointer is absent above. Native useSpuBuffer is ignored,
  // false for this CPU path; both plane arrays remain at the same offset.
  const auto planes=U32(data,old+0x58);Need(planes && planes<=1000000,"Invalid convex plane count");
  const auto plane_data=Pointer(old+0x54);for(uint64_t i=0;i<uint64_t(planes)*4;++i)F32(data,plane_data+uint32_t(i*4));
  Array(old+0x54,target+0x54,16,1000000);return target;
 }
 uint32_t Box(uint32_t old) {
  if(auto found=boxes.find(old);found!=boxes.end())return found->second;
  Need(resolving_boxes.size()<32 && resolving_boxes.insert(old).second,"Cyclic/deep authored box translation graph");
  const auto type=source.Class({2,old});const bool translated=type=="hkpConvexTranslateShape";
  Need(translated || type=="hkpBoxShape","Unsupported authored boxed body child");Range(old,0x30);
  // Both independently recovered HK7 and Xbox2011 hkClass closures have the
  // same48-byte box/translate layouts. Shape type, container vtable and cached
  // childShapeSize are serialization-ignored native finish-loaded state.
  const auto target=output.Allocate(0x30);
  output.virtuals.push_back({target,0,translated?12u:11u});
  W32(output.data,target+8,U32(data,old+8));
  Need(F32(data,old+0x10)>=0,"Negative authored box/translation radius");
  W32(output.data,target+0x10,U32(data,old+0x10));
  ScalarVector(old+0x20,target+0x20,4);
  for(const auto& [key,to]:source.pointers)if(uint32_t(key>>32)==2){
   const auto from=uint32_t(key);
   if(from>=old && from<old+0x30)
    Need(translated && from==old+0x18 && to.section==2,"Unsupported pointer in authored box/translation shape");
  }
  if(translated){const auto child=Pointer(old+0x18);const auto converted=Box(child);output.Pointer(target+0x18,converted,true);}
  else for(unsigned i=0;i<3;++i)Need(F32(data,old+0x20+i*4)>=0,"Negative authored box half-extent");
  resolving_boxes.erase(old);boxes.emplace(old,target);return target;
 }
 uint32_t MoppList(uint32_t old) {
  Need(source.Class({2,old})=="hkpMoppBvTreeShape","Compound root must be authored MOPP");Range(old,0x40);
  const auto old_code=Pointer(old+0x14),old_list=Pointer(old+0x34);
  Need(source.Class({2,old_code})=="hkpMoppCode" && source.Class({2,old_list})=="hkpListShape","Unsupported compound MOPP child");
  Range(old_code,0x30);Range(old_list,0x70);
  const auto root=output.Allocate(0x40),code=output.Allocate(0x30),list=output.Allocate(0x70);
  output.virtuals.push_back({root,0,0});output.virtuals.push_back({code,0,1});output.virtuals.push_back({list,0,10});
  W32(output.data,root+8,U32(data,old+8));output.data[root+0x10]=B8(data,old+0x10);
  output.Pointer(root+0x14,code,true);output.Pointer(root+0x34,list,true);
  ScalarVector(old_code+0x10,code+0x10,4);Array(old_code+0x20,code+0x20,1,16*1024*1024);
  Need(U32(data,old_code+0x24)>0,"Empty compound MOPP program");output.data[code+0x2c]=B8(data,old_code+0x2c);
  W32(output.data,list+8,U32(data,old_list+8));output.data[list+0x14]=B8(data,old_list+0x14);output.data[list+0x15]=B8(data,old_list+0x15);
  // Reflected list layout is unchanged; childInfo v1 replaces the old ignored
  // int32 shapeSize with shapeInfo/shapeSize halfwords. Initialize new runtime
  // cache fields instead of copying old ignored values into new flags.
  Copy(output.data,list+0x24,Range(old_list+0x24,0x4c));
  for(unsigned i=0;i<8;++i)F32(data,old_list+0x30+i*4);
  const auto count=U32(data,old_list+0x1c),children=Pointer(old_list+0x18);
  Need(count>0 && count<=256 && U16(data,old_list+0x26)<=count,"Compound list child budget");
  Range(children,uint64_t(count)*16);const auto native_children=output.Allocate(uint64_t(count)*16);output.Array(list+0x18,native_children,count);
  std::set<uint32_t> reachable{old,old_code,old_list};
  for(uint32_t i=0;i<count;++i){const auto child=Pointer(children+i*16),converted=Convex(child);reachable.insert(child);
   output.Pointer(native_children+i*16,converted,true);W32(output.data,native_children+i*16+4,U32(data,children+i*16+4));
  }
  for(const auto& [location,type]:source.classes)if(uint32_t(location>>32)==2 && uint32_t(location)>=old)
   Need(reachable.contains(uint32_t(location)),"Unreachable authored compound shape would be lost");
  return root;
 }
 void Triangle(uint32_t old,uint32_t target,uint32_t part_index,bool record=true) {
  Range(old,0x60);Need(B8(data,old)==0,"Jigsaw triangle part has wrong type");
  const auto triangles=U32(data,old+0x14),vertices=U32(data,old+0x20),vs=U32(data,old+0x1c),is=U32(data,old+0x44);
  const auto width=B8(data,old+0x48),flip=B8(data,old+0x49);
  Need(triangles && triangles<=1000000 && vertices && vertices<=1000000 && vs>=12 && vs<=256 &&
      is>=3*width && is<=256 && width>=1 && width<=3 && flip<=1,"Invalid authored triangle layout");
  const uint32_t index_width=width==3?4:width;
  Need(!record || uint64_t(info.triangles.size())+triangles<=1000000,"Cumulative Jigsaw triangle proof budget");
  Need(is>=3*index_width && vs<=65535 && is<=65535,"Authored triangle stride exceeds native fields");
  Need(uint64_t(triangles)<=(uint64_t(1)<<(32-info.key_bits)),"Jigsaw triangle keys exceed subpart keyspace");
  SubpartBase(old,target,triangles,U32(data,old+0x50));
  W32(output.data,target+0x14,triangles);W32(output.data,target+0x1c,vertices);W16(output.data,target+0x24,uint16_t(vs));
  W32(output.data,target+0x28,U32(data,old+0x4c));W16(output.data,target+0x2c,uint16_t(is));output.data[target+0x2e]=width;output.data[target+0x2f]=flip;
  ScalarVector(old+0x30,target+0x30,4);IdentityTransform(output.data,target);
  nif::View vertex_data,index_data;
  if(bindings){
   Need(part_index<bindings->triangle_vertices.size(),"Missing Jigsaw external triangle binding");
   const auto& v=bindings->vertices.at(bindings->triangle_vertices[part_index]);const auto& i=bindings->indices.at(bindings->triangle_indices[part_index]);
   Need(vs==12 && v.count==vertices && v.bytes.size()==uint64_t(vertices)*vs,"Jigsaw external vertex layout mismatch");
   const uint32_t format=index_width==1?0x00010105:index_width==2?0x00010215:0x00010425;
   Need(i.format==format && uint64_t(triangles)*is<=i.bytes.size(),"Jigsaw external index layout mismatch");
   vertex_data=v.bytes;index_data=i.bytes.first(size_t(triangles)*is);
  }else{
   const auto va=Pointer(old+0x18,true),ia=Pointer(old+0x40,true);
   Need(va!=UINT32_MAX,"Jigsaw collision requires an explicit binding to external mesh vertices");
   Need(ia!=UINT32_MAX,"Jigsaw collision requires an explicit binding to external mesh indices");
   vertex_data=Range(va,uint64_t(vertices)*vs);index_data=Range(ia,uint64_t(triangles)*is);
  }
  const auto vcopy=BoundBuffer(vertex_data),icopy=BoundBuffer(index_data);
  output.Pointer(target+0x18,vcopy);output.Pointer(target+0x20,icopy);
  for(uint32_t t=0;t<triangles;++t){JigsawTriangleProof proof;proof.key=(part_index<<(32-info.key_bits))|t;
   for(unsigned j=0;j<3;++j){const auto at=t*is+j*index_width;const auto index=index_width==1?B8(index_data,at):index_width==2?U16(index_data,at):U32(index_data,at);Need(index<vertices,"Jigsaw authored triangle index");
    for(unsigned k=0;k<3;++k)proof.vertices[j][k]=F32(vertex_data,index*vs+k*4);}
   if(flip && (t&1))std::swap(proof.vertices[1],proof.vertices[2]);if(record)info.triangles.push_back(proof);
  }
 }
 void Shapes(uint32_t old,uint32_t target) {
  Range(old,0x40);Need(B8(data,old)==1,"Jigsaw convex part has wrong type");
  const auto count=U32(data,old+0x18);Need(count && count<=1024,"Jigsaw convex child count");
  SubpartBase(old,target,count,0);ScalarVector(old+0x20,target+0x20,8);
  const auto children=Pointer(old+0x14),array=output.Allocate(uint64_t(count)*4);output.Array(target+0x14,array,count);
  for(uint32_t i=0;i<count;++i){const auto converted=Convex(Pointer(children+i*4));output.Pointer(array+i*4,converted,true);}
 }
 void Mesh(uint32_t old,uint32_t target) {
  Need(source.Class({2,old})=="hkpExtendedMeshShape","Jigsaw MOPP child must be a plain authored ExtendedMesh");Range(old,0xf0);
  for(unsigned i=0;i<3;++i)Need(F32(data,old+0x80+i*4)==1.f,"Nonidentity Jigsaw global scaling needs a native transform adapter");
  info.key_bits=U32(data,old+0xb0);Need(info.key_bits>1 && info.key_bits<=31,"Jigsaw key bit count");
  info.triangle_parts=U32(data,old+0xb8);info.convex_parts=U32(data,old+0xc4);
  if(bindings)Need(bindings->triangle_vertices.size()==info.triangle_parts && bindings->convex_pieces.size()==info.convex_parts,"Jigsaw physics part/binding counts differ");
  Need((info.triangle_parts || info.convex_parts) && info.triangle_parts<=1024 && info.convex_parts<=1024 &&
      uint64_t(std::max(info.triangle_parts,info.convex_parts))<=(uint64_t(1)<<(info.key_bits-1)),"Jigsaw subpart budget/keyspace");
  W32(output.data,target+8,U32(data,old+8));output.data[target+0x14]=B8(data,old+0x14);output.data[target+0x15]=B8(data,old+0x15);
  // This is populated even when the serialized triangles array has six parts;
  // retain its geometry rather than treating it as an unused default member.
  if(info.triangle_parts)Triangle(old+0x20,target+0x20,0,false);
  else IdentityTransform(output.data,target+0x20); // Inactive default; no triangle geometry exists.
  ScalarVector(old+0x90,target+0x90,8);
  W32(output.data,target+0xb4,info.key_bits);output.data[target+0xdc]=B8(data,old+0xd8);
  W32(output.data,target+0xe0,U32(data,old+0xdc));W32(output.data,target+0xe4,U32(data,old+0xe0));
  Need(F32(data,old+0xe4)>=0,"Invalid Jigsaw triangle radius");W32(output.data,target+0xe8,U32(data,old+0xe4));
  if(info.triangle_parts){const auto parts=Pointer(old+0xb4),triangles=output.Allocate(uint64_t(info.triangle_parts)*0x70);
   output.Array(target+0xb8,triangles,info.triangle_parts);
   for(uint32_t i=0;i<info.triangle_parts;++i)Triangle(parts+i*0x60,triangles+i*0x70,i);
  }else output.Empty(target+0xb8);
  if(info.convex_parts){const auto shapes=Pointer(old+0xc0),new_shapes=output.Allocate(uint64_t(info.convex_parts)*0x40);output.Array(target+0xc4,new_shapes,info.convex_parts);
   for(uint32_t i=0;i<info.convex_parts;++i)Shapes(shapes+i*0x40,new_shapes+i*0x40);
  }else output.Empty(target+0xc4);
  Array(old+0xcc,target+0xd0,2,1000000);
 }
 void Convert() {
  const auto root=output.Allocate(0x40),code=output.Allocate(0x30),mesh=output.Allocate(0xf0);
  output.virtuals={{root,0,0},{code,0,1},{mesh,0,2}};
  Range(0,0x40);const auto old_code=Pointer(0x14),old_mesh=Pointer(0x34);
  Need(source.Class({2,old_code})=="hkpMoppCode","Missing authored Jigsaw MOPP code");Range(old_code,0x30);
  W32(output.data,8,U32(data,8));output.data[0x10]=B8(data,0x10);
  output.Pointer(0x14,code,true);output.Pointer(0x34,mesh,true);
  ScalarVector(old_code+0x10,code+0x10,4);info.mopp_bytes=U32(data,old_code+0x24);
  Need(info.mopp_bytes && info.mopp_bytes<=16*1024*1024,"Jigsaw MOPP bytecode budget");
  Array(old_code+0x20,code+0x20,1,16*1024*1024);output.data[code+0x2c]=B8(data,old_code+0x2c);
  Mesh(old_mesh,mesh);
  for(const auto& [location,type]:source.classes)if(uint32_t(location>>32)==2){
   const auto offset=uint32_t(location);
   Need(offset==0 || offset==old_code || offset==old_mesh || convex.contains(offset),"Unreachable authored Jigsaw object would be lost");
  }
 }
 Bytes Finish(unsigned root_type=0) {
  output.Allocate(0);const uint32_t local=uint32_t(output.data.size());
  auto emit=[&](const auto& rows){for(const auto& row:rows)for(uint32_t word:row){const auto p=output.data.size();output.data.resize(p+4);W32(output.data,p,word);}while(output.data.size()%16)output.data.push_back(0xff);};
  emit(output.local);const uint32_t global=uint32_t(output.data.size());emit(output.global);const uint32_t virtuals=uint32_t(output.data.size());
  // Exact native2011 signatures from original Xbox PAKs, NOT old signatures
  // relabeled by a version string. See native-jigsaw-signatures/embedded-evidence.
  const std::array<std::pair<std::string_view,uint32_t>,15> types={{{"hkpMoppBvTreeShape",0x802b729d},{"hkpMoppCode",0x924c2661},{"hkpExtendedMeshShape",0x94167784},{"hkpConvexVerticesShape",0x8cd72f94},
    {"hkRootLevelContainer",0x2772c11e},{"hkxScene",0x5f673ddd},{"hkMemoryResourceContainer",0x4762f92a},
    {"hkpPhysicsData",0xc2a461e4},{"hkpPhysicsSystem",0xff724c17},{"hkpRigidBody",0x0921dc44},
    // Retail tcy_autotopia_02a_initial.pak / TCY_GuardianGeyserPM_01a.hkx.
    {"hkpListShape",0xaa54efa5},
    // Xbox _Dynamic.pak/gameobjects/pickups/GenPickupCollision.hkx confirms
    // Box. Translate's complete source/target reflected closure is identical;
    // retain its schema signature (see em1-box-body-layout-audit.json).
    {"hkpBoxShape",0x3444d2d5},{"hkpConvexTranslateShape",0x5ba0a5f7},
    {"hkpStorageExtendedMeshShape",0xd424d9b6},{"hkpStorageExtendedMeshShapeMeshSubpartStorage",0x5aad4de6}}};
  const auto type_count=std::any_of(output.virtuals.begin(),output.virtuals.end(),[](const auto& row){return row[2]==11 || row[2]==12;})?size_t(13):size_t(11);
  Bytes names;std::array<uint32_t,15> offsets{};
  for(size_t i=0;i<types.size();++i){
   const bool used=std::any_of(output.virtuals.begin(),output.virtuals.end(),[&](const auto& row){return row[2]==i;});
   if(i>=type_count && !used)continue;
   const auto at=names.size();names.resize(at+5);W32(names,at,types[i].second);names[at+4]=9;offsets[i]=uint32_t(names.size());names.insert(names.end(),types[i].first.begin(),types[i].first.end());names.push_back(0);
  }while(names.size()%16)names.push_back(0xff);
  for(auto& row:output.virtuals)row[2]=offsets[row[2]];emit(output.virtuals);const auto end=uint32_t(output.data.size());
  const uint32_t names_at=208,data_at=names_at+uint32_t(names.size());Bytes result(uint64_t(data_at)+end);
  Need(root_type<types.size() && offsets[root_type],"Native collision root type range");
  W32(result,0,0x57e0e057);W32(result,4,0x10c0c010);W32(result,12,9);result[16]=4;result[19]=1;W32(result,20,3);W32(result,24,2);W32(result,36,offsets[root_type]);
  const char version[]="hk_2011.2.0-r1";std::memcpy(result.data()+40,version,sizeof(version));result[55]=0xff;W32(result,60,UINT32_MAX);
  auto section=[&](unsigned index,const char* name,std::array<uint32_t,7> fields){const auto at=64+index*48;std::memcpy(result.data()+at,name,std::strlen(name));for(unsigned i=0;i<7;++i)W32(result,at+20+i*4,fields[i]);};
  const auto ns=uint32_t(names.size());section(0,"__classnames__",{names_at,ns,ns,ns,ns,ns,ns});section(1,"__types__",{data_at,0,0,0,0,0,0});section(2,"__data__",{data_at,local,global,virtuals,end,end,end});
  Copy(result,names_at,names);Copy(result,data_at,output.data);return result;
 }
};
struct JigsawSpan {size_t length_at=0,blob_at=0,blob_size=0;uint32_t pieces=0;};
JigsawSpan Embedded(nif::View block,uint32_t blocks) {
 nif::Cursor c{block,0,true};c.Word();const auto extras=c.Word();Need(extras<=blocks,"Jigsaw extra-data count");c.Take(uint64_t(extras)*4);c.Word();
 c.Short();c.Take(52);const auto properties=c.Word();Need(properties<=blocks,"Jigsaw property count");c.Take(uint64_t(properties)*4);c.Word();
 const auto children=c.Word();Need(children<=blocks,"Jigsaw child count");c.Take(uint64_t(children)*4);const auto effects=c.Word();Need(effects<=blocks,"Jigsaw effect count");c.Take(uint64_t(effects)*4);
 Need(c.Word()==4,"Only authored Jigsaw serialization version4 is adapted");const auto pieces=c.Word();Need(pieces && pieces<=4096,"Jigsaw piece budget");
 for(uint32_t i=0;i<pieces;++i){Need(c.Word()==4,"Only authored Jigsaw piece version4 is adapted");c.Word();c.Short();for(unsigned list=0;list<2;++list){const auto count=c.Short();Need(count<=blocks,"Jigsaw piece link count");c.Take(uint64_t(count)*4);}const auto keys=c.Word();Need(keys<=1000000,"Jigsaw key count");c.Take(uint64_t(keys)*4);c.Take(12);}
 const auto links=c.Word();Need(links<=1000000,"Jigsaw adjacency count");c.Take(uint64_t(links)*2);
 Need(c.Word()==0 && c.Word()==0,"Legacy split Jigsaw physics requires a separate adapter");c.Take(48);
 const auto length_at=c.at;const auto size=c.Word();const auto blob_at=c.at;Need(size && size<=32*1024*1024,"Jigsaw aggregate blob budget");c.Take(size);return {length_at,blob_at,size,pieces};
}
} // namespace

bool em2::em1::ConvertJigsawAggregate(std::span<const uint8_t> original,ConvertedJigsawAggregate& result,std::string& error) {
 result={};error.clear();try{Converter converter(original);converter.Convert();auto pack=converter.Finish();Pack check(pack,false);
  Need(check.Class({2,0})=="hkpMoppBvTreeShape","Converted Jigsaw root class mismatch");
  result.pack=std::move(pack);result.info=std::move(converter.info);return true;
 }catch(const std::exception& exception){result={};error=exception.what();return false;}
}
bool em2::em1::ConvertConvexBodyShape(std::span<const uint8_t> original,ConvertedConvexBodyShape& result,std::string& error){
 result={};error.clear();try{
  Converter c(original,true);uint32_t body=UINT32_MAX;
  for(const auto& [key,name]:c.source.classes)if(name=="hkpRigidBody"){
   Need(uint32_t(key>>32)==2 && body==UINT32_MAX,"Convex body adapter requires exactly one data-section body");body=uint32_t(key);
  }
  Need(body!=UINT32_MAX,"Convex body adapter found no rigid body");c.Range(body,0x220);
  const auto shape=c.Pointer(body+0x10);Need(c.Convex(shape)==0,"Native convex root offset");
  ConvertedConvexBodyShape prepared;prepared.source_body_offset=body;prepared.motion_type=B8(c.data,body+0xe8);
  const auto planes=c.Pointer(shape+0x54),count=U32(c.data,shape+0x58);
  for(uint32_t i=0;i<count;++i){std::array<float,4> p{};for(unsigned j=0;j<4;++j)p[j]=F32(c.data,planes+i*16+j*4);prepared.planes.push_back(p);}
  prepared.vertices=std::move(c.info.convex_vertices);prepared.pack=c.Finish(3);
  Pack check(prepared.pack,false);Need(check.Class({2,0})=="hkpConvexVerticesShape","Converted convex root class mismatch");
  result=std::move(prepared);return true;
 }catch(const std::exception& exception){error=exception.what();return false;}
}
namespace em2::em1 {
enum class BodyShapeKind {Convex,Compound,Box};
static void MigrateNativeBody(Converter& c,uint32_t source_body,uint32_t body){
  c.Range(source_body,0x220);
  // Actual source/target hkClass layouts are recorded in native-gear-body-layout-audit.json.
  // Expand debug thread state, move name/properties, remove obsolete ai/tree data.
  std::fill(c.output.data.begin()+body+0x6c,c.output.data.begin()+body+0x78,0);
  Copy(c.output.data,body+0x78,c.data.subspan(source_body+0x74,16));
  Copy(c.output.data,body+0x88,c.data.subspan(source_body+0x8c,0x54));
  W16(c.output.data,body+0x8a,0); // new rollingFrictionMultiplier, absent in HK7
  std::fill(c.output.data.begin()+body+0xdc,c.output.data.begin()+body+0xe0,0);
  W32(c.output.data,body+0x218,0); // new native npData
  const auto motion=source_body+0xe0,state=motion+0x10,target_state=body+0xf0;
  // Reflected HK7/2011 fixed, box and sphere motion subclasses add no fields
  // to the same 288-byte hkpMotion base. The shared hkMotionState migration
  // below applies to either; other motion subclasses remain unsupported.
  const auto motion_type=B8(c.data,motion+8);
  Need(motion_type==2 || motion_type==3 || motion_type==5,"Only authored sphere/box/fixed motion layouts are audited");
  if(motion_type==2)Need(U32(c.data,motion+0xc0)==U32(c.data,motion+0xc4) && U32(c.data,motion+0xc4)==U32(c.data,motion+0xc8),"Authored sphere motion must retain isotropic inverse inertia");
  for(unsigned at=0x10;at<0xbc;at+=4)F32(c.data,motion+at);
  for(unsigned at=0xc0;at<0xf0;at+=4){
   const auto value=F32(c.data,motion+at);
   if(at<0xd0)Need(value>=0,"Negative authored inverse inertia/mass");
  }
  Need(F32(c.data,source_body+0x90)>=0 && F32(c.data,source_body+0x94)>=0,"Negative authored friction/restitution");
  for(unsigned field: {0xa4u,0xa8u})Need(F32(c.data,state+field)>=0,"Negative authored damping");
  // hkHalf is the high word of IEEE float, not IEEE binary16. Conversion loses
  // only native-format precision; the original full-precision asset is retained.
  W16(c.output.data,target_state+0xa4,uint16_t(U32(c.data,state+0xa4)>>16));
  W16(c.output.data,target_state+0xa6,uint16_t(U32(c.data,state+0xa8)>>16));
  W16(c.output.data,target_state+0xa8,0x3f80); // timeFactor 1, absent in HK7
  for(unsigned i=0;i<3;++i)c.output.data[target_state+0xaa+i]=B8(c.data,state+0xac+i);
  std::fill(c.output.data.begin()+target_state+0xad,c.output.data.begin()+target_state+0xb0,0);
}
static bool ConvertBodyResource(std::span<const uint8_t> original,ConvertedConvexBodyResource& result,std::string& error,BodyShapeKind kind){
 result={};error.clear();try{
  const bool compound=kind==BodyShapeKind::Compound,boxed=kind==BodyShapeKind::Box;
  Converter c(original,true);uint32_t body=UINT32_MAX,shape=UINT32_MAX;
  std::map<std::string,unsigned> types={{"hkRootLevelContainer",4},{"hkxScene",5},{"hkMemoryResourceContainer",6},
    {"hkpPhysicsData",7},{"hkpPhysicsSystem",8},{"hkpRigidBody",9},{"hkpConvexVerticesShape",3}};
  if(compound){types.emplace("hkpMoppBvTreeShape",0);types.emplace("hkpMoppCode",1);types.emplace("hkpListShape",10);}
  if(boxed){types.erase("hkpConvexVerticesShape");types.emplace("hkpBoxShape",11);types.emplace("hkpConvexTranslateShape",12);}
  std::set<std::string> seen;
  for(const auto& [key,name]:c.source.classes){
   Need(uint32_t(key>>32)==2 && types.contains(name),"Unsupported body-resource object");
   const bool unique=seen.insert(name).second;
   Need(unique || (compound && name=="hkpConvexVerticesShape") || (boxed && name=="hkpConvexTranslateShape"),"Duplicate body-resource object");
   if(name=="hkpRigidBody")body=uint32_t(key);
   if(name==(compound?"hkpMoppBvTreeShape":"hkpConvexVerticesShape"))shape=uint32_t(key);
  }
  if(boxed && body!=UINT32_MAX){
   shape=c.Pointer(body+0x10);
   Need(seen.contains("hkpBoxShape") && seen.size()==types.size()-(seen.contains("hkpConvexTranslateShape")?0u:1u),"Incomplete boxed body resource");
  }else Need(seen.size()==types.size(),"Incomplete convex body resource");
  Need(body!=UINT32_MAX && shape!=UINT32_MAX,"Incomplete authored body/shape closure");
  c.Range(body,0x220);Need(body+0x220<=shape && c.Pointer(body+0x10)==shape,"Unsupported convex body ordering");
  // Metadata schemas are identical between the two original executables. Keep
  // their strings/arrays and offsets intact, replacing the shape closure only.
  c.output.Allocate(shape);Copy(c.output.data,0,c.data.first(shape));
  for(const auto& [key,name]:c.source.classes){
   const auto at=uint32_t(key);if(at>=shape)continue;
   Need(at<shape,"Body metadata follows shape storage");
   c.output.virtuals.push_back({at,0,types.at(name)});
  }
  const auto target_shape=boxed?c.Box(shape):compound?c.MoppList(shape):c.Convex(shape);
  if(boxed)for(const auto& [key,name]:c.source.classes)if(name=="hkpBoxShape" || name=="hkpConvexTranslateShape")
   Need(c.boxes.contains(uint32_t(key)),"Unreachable authored box/translation object would be dropped");
  for(const auto& [key,to]:c.source.pointers){
   Need(uint32_t(key>>32)==2 && to.section==2,"Cross-section body-resource pointer");
   auto from=uint32_t(key);if(from>=shape)continue;
   Need(to.offset<shape || to.offset==shape,"Unsupported body metadata references shape storage");
   if(from>=body && from<body+0x220){
    Need(from==body+0x10 || from==body+0x74,"Unsupported authored body pointer dependency");
    if(from==body+0x74)from=body+0x78;
   }
   c.output.Pointer(from,to.offset==shape?target_shape:to.offset);
  }
  MigrateNativeBody(c,body,body);
  auto pack=c.Finish(4);Pack check(pack,false);
  Need(check.Class({2,0})=="hkRootLevelContainer" && check.Class({2,body})=="hkpRigidBody","Native body class closure mismatch");
  result.pack=std::move(pack);result.body_offset=body;result.body_offsets={body};return true;
 }catch(const std::exception& exception){error=exception.what();return false;}
}
bool ConvertConvexBodyResource(std::span<const uint8_t> original,ConvertedConvexBodyResource& result,std::string& error){return ConvertBodyResource(original,result,error,BodyShapeKind::Convex);}
bool ConvertCompoundBodyResource(std::span<const uint8_t> original,ConvertedConvexBodyResource& result,std::string& error){return ConvertBodyResource(original,result,error,BodyShapeKind::Compound);}
bool ConvertBoxBodyResource(std::span<const uint8_t> original,ConvertedConvexBodyResource& result,std::string& error){return ConvertBodyResource(original,result,error,BodyShapeKind::Box);}
bool ConvertConvexBodiesResource(std::span<const uint8_t> original,ConvertedConvexBodyResource& result,std::string& error){
 result={};error.clear();try{
  Converter c(original,true);
  const std::map<std::string,unsigned> types={{"hkRootLevelContainer",4},{"hkxScene",5},{"hkMemoryResourceContainer",6},
    {"hkpPhysicsData",7},{"hkpPhysicsSystem",8},{"hkpRigidBody",9},{"hkpConvexVerticesShape",3}};
  std::map<uint32_t,uint32_t> bodies;std::set<uint32_t> shapes;std::set<std::string> metadata;
  for(const auto& [key,name]:c.source.classes){
   Need(uint32_t(key>>32)==2 && types.contains(name),"Unsupported multi-body resource class");
   const auto at=uint32_t(key);
   if(name=="hkpRigidBody"){Need(bodies.size()<1024,"Multi-body resource exceeds body budget");c.Range(at,0x220);bodies.emplace(at,0);}
   else if(name=="hkpConvexVerticesShape"){c.Range(at,0x70);shapes.insert(at);}
   else Need(metadata.insert(name).second,"Duplicate multi-body metadata object");
  }
  Need(!bodies.empty() && !shapes.empty() && metadata.size()==5,"Incomplete multi-body resource closure");
  for(const auto& [key,name]:c.source.classes){const auto at=uint32_t(key);
   for(const auto& [body,relocated]:bodies)Need(at==body || at<body || uint64_t(at)>=uint64_t(body)+0x220,"Overlapping authored body objects");
   for(auto shape:shapes)Need(at==shape || at<shape || uint64_t(at)>=uint64_t(shape)+0x70,"Overlapping authored convex objects");
  }
  // Keep the original non-object backing storage at its exact offsets. Only
  // registered bodies/shapes move; names, system arrays and metadata retain
  // their bytes, with every active pointer fixed up to the relocated object.
  c.output.Allocate(c.data.size());Copy(c.output.data,0,c.data);
  for(auto& [old,relocated]:bodies){
   relocated=c.output.Allocate(0x220);Copy(c.output.data,relocated,c.Range(old,0x220));MigrateNativeBody(c,old,relocated);
   c.output.virtuals.push_back({relocated,0,9});
  }
  for(const auto& [old,relocated]:bodies){
   const auto shape=c.Pointer(old+0x10);Need(shapes.contains(shape),"Body references a foreign convex shape");c.Convex(shape);
  }
  Need(c.convex.size()==shapes.size(),"Unreachable authored convex shape would be dropped");
  for(const auto& [key,name]:c.source.classes)if(name!="hkpRigidBody" && name!="hkpConvexVerticesShape")
   c.output.virtuals.push_back({uint32_t(key),0,types.at(name)});
  for(const auto& [key,to]:c.source.pointers){
   Need(uint32_t(key>>32)==2 && to.section==2,"Cross-section multi-body pointer");
   uint32_t from=uint32_t(key),target=to.offset;bool inactive_shape=false;
   for(auto shape:shapes)if(from>=shape && uint64_t(from)<uint64_t(shape)+0x70){inactive_shape=true;break;}
   if(inactive_shape)continue; // Convex() rebuilt this object's complete graph.
   for(const auto& [old,relocated]:bodies)if(from>=old && uint64_t(from)<uint64_t(old)+0x220){
    const auto field=from-old;
    Need(field==0x10 || field==0x74 || field==0x78,"Unsupported multi-body pointer field");
    from=relocated+(field==0x74?0x78:field==0x78?0x7c:field);break;
   }
   if(auto found=bodies.find(target);found!=bodies.end())target=found->second;
   else if(auto found=c.convex.find(target);found!=c.convex.end())target=found->second;
   else{
    for(const auto& [old,relocated]:bodies)Need(target<old || uint64_t(target)>=uint64_t(old)+0x220,"Interior body reference requires a separate migration");
    for(auto shape:shapes)Need(target<shape || uint64_t(target)>=uint64_t(shape)+0x70,"Interior convex reference requires a separate migration");
   }
   c.output.Pointer(from,target);
  }
  auto pack=c.Finish(4);Pack check(pack,false);
  Need(check.Class({2,0})=="hkRootLevelContainer","Multi-body root changed");
  ConvertedConvexBodyResource ready;
  for(const auto& [old,relocated]:bodies){Need(check.Class({2,relocated})=="hkpRigidBody","Relocated body class missing");ready.body_offsets.push_back(relocated);}
  ready.body_offset=ready.body_offsets.front();ready.pack=std::move(pack);result=std::move(ready);return true;
 }catch(const std::exception& exception){error=exception.what();return false;}
}
bool ConvertStaticTriangleBodyResource(std::span<const uint8_t> original,ConvertedConvexBodyResource& result,std::string& error,bool accelerated){
 result={};error.clear();try{
  Converter c(original,true);
  for(const auto& section:c.source.sections)Need(section.exports==section.imports && section.imports==section.end,
      "External static pack imports/exports are unsupported");
  const std::map<std::string,unsigned> metadata_types={{"hkRootLevelContainer",4},{"hkxScene",5},
    {"hkMemoryResourceContainer",6},{"hkpPhysicsData",7},{"hkpPhysicsSystem",8}};
  const std::map<std::string,uint32_t> geometry_sizes={{"hkpMoppBvTreeShape",0x40},{"hkpMoppCode",0x30},
    {"hkpStorageExtendedMeshShape",0x108},{"hkpStorageExtendedMeshShapeMeshSubpartStorage",0x60}};
  std::map<uint32_t,uint32_t> bodies;std::set<std::string> metadata;
  std::set<uint32_t> geometry_objects,reachable;
  uint32_t system=UINT32_MAX,physics=UINT32_MAX;
  for(const auto& [key,name]:c.source.classes){
   Need(uint32_t(key>>32)==2,"Cross-section static resource object");const auto at=uint32_t(key);
   if(name=="hkpRigidBody"){
    Need(bodies.size()<1024,"Static body count budget");c.Range(at,0x220);
    Need(B8(c.data,at+0xe8)==5,"Static triangle resource requires fixed motions");bodies.emplace(at,0);
   }else if(auto type=metadata_types.find(name);type!=metadata_types.end()){
    Need(metadata.insert(name).second,"Duplicate static metadata object");
    if(name=="hkpPhysicsSystem")system=at;
    if(name=="hkpPhysicsData")physics=at;
    if(name=="hkRootLevelContainer")c.Range(at,12);
    if(name=="hkpPhysicsSystem")c.Range(at,68);
    if(name=="hkpPhysicsData")c.Range(at,24);
   }else{
    Need(geometry_sizes.contains(name),"Unsupported static triangle resource object");
    c.Range(at,geometry_sizes.at(name));geometry_objects.insert(at);
   }
  }
  Need(!bodies.empty() && metadata.size()==5 && !geometry_objects.empty(),"Incomplete static triangle resource closure");
  Need(c.Pointer(physics+8,true)==UINT32_MAX,"Static resource worldCinfo is unsupported");
  Need(U32(c.data,physics+16)==1,"Static resource must retain exactly one audited physics system");
  const auto systems=c.Pointer(physics+12);c.Range(systems,4);
  Need(c.Pointer(systems)==system,"Static PhysicsData points to a foreign system");
  for(const uint32_t field:{20u,32u,44u})Need(U32(c.data,system+field+4)==0 && c.Pointer(system+field,true)==UINT32_MAX,
      "Static resource constraints/actions/phantoms are unsupported");
  for(const auto& [at,name]:c.source.classes)for(const auto& [body,target]:bodies){
   const auto offset=uint32_t(at);Need(offset==body || offset<body || uint64_t(offset)>=uint64_t(body)+0x220,"Static objects overlap an authored body");
  }
  std::set<uint32_t> system_bodies;const auto count=U32(c.data,system+0xc);
  Need(count==bodies.size(),"Static system body array does not own every body");const auto body_array=c.Pointer(system+8);c.Range(body_array,uint64_t(count)*4);
  for(uint32_t i=0;i<count;++i){const auto body=c.Pointer(body_array+i*4);
   Need(bodies.contains(body) && system_bodies.insert(body).second,"Invalid/duplicate static system body reference");}
  struct RemovedRange{uint32_t at,size;};std::vector<RemovedRange> removed;
  auto remove=[&](uint32_t at,uint64_t size,bool object){
   c.Range(at,size);Need(size<=UINT32_MAX,"Static geometry range overflow");
   for(const auto& [key,name]:c.source.classes){const auto other=uint32_t(key);
    if(uint64_t(other)>=at && uint64_t(other)<uint64_t(at)+size)
     Need(object && other==at && geometry_objects.contains(other),"Static geometry range overlaps another object");
   }
   for(const auto& [body,target]:bodies)Need(uint64_t(at)+size<=body || uint64_t(body)+0x220<=at,"Static geometry overlaps body storage");
   removed.push_back({at,uint32_t(size)});
  };
  auto is_removed=[&](uint32_t offset){return std::any_of(removed.begin(),removed.end(),[&](const auto& r){return offset>=r.at && uint64_t(offset)<uint64_t(r.at)+r.size;});};
  auto global_sources=[](const Pack& file){std::set<uint32_t> fields;
   for(uint32_t section=0;section<file.sections.size();++section){const auto& s=file.sections[section];
    for(uint32_t p=s.global;p+12<=s.virtuals;p+=12){const auto from=U32(file.bytes,s.start+p);if(from==UINT32_MAX)break;
     Need(section==2 && U32(file.bytes,s.start+p+4)==2,"Cross-section static global fixup");fields.insert(from);
    }
   }return fields;
  };
  const auto old_globals=global_sources(c.source);
  for(const auto& [from,to]:c.source.pointers)Need(uint32_t(from>>32)==2 && to.section==2,"Cross-section static resource pointer");
  // Preserve the complete original backing store, including scene metadata,
  // names, property arrays and system arrays, at its original offsets. Native
  // bodies/shapes are appended and only registered references are relocated.
  c.output.Allocate(c.data.size());Copy(c.output.data,0,c.data);
  for(auto& [old,target]:bodies){target=c.output.Allocate(0x220);Copy(c.output.data,target,c.Range(old,0x220));
   MigrateNativeBody(c,old,target);c.output.virtuals.push_back({target,0,9});}
  AuthoredStaticCollision reconstructed;std::string why;
  Need(ReconstructAuthoredStaticCollision(original,reconstructed,why),why.empty()?"Static geometry reconstruction failed":why.c_str());
  Need(reconstructed.bodies.size()==bodies.size(),"Static reconstruction body count differs");
  if(accelerated)Need(AccelerateAuthoredStaticCollision(reconstructed,why),why.empty()?"Static MOPP reconstruction failed":why.c_str());
  struct ImportedShape{Bytes bytes;uint32_t mesh_target,root_target;};std::map<uint32_t,ImportedShape> meshes;
  std::map<uint32_t,uint32_t> shape_targets;
  for(const auto& body:reconstructed.bodies){
   Need(bodies.contains(body.source_offset) && body.motion_type==5 && body.accelerated==accelerated,"Static source body provenance mismatch");
   Need((accelerated?body.original_mopp_user_data:body.shape_user_data)==0,
       accelerated?"Native static component ownership rejected: active MOPP root has nonzero authored userdata":
                   "Native static component ownership rejected: active storage mesh root has nonzero authored userdata");
   const auto root=c.Pointer(body.source_offset+0x10);
   Need(c.source.Class({2,root})=="hkpMoppBvTreeShape","Static body root is not original MOPP");
   const auto code=c.Pointer(root+0x14),mesh=c.Pointer(root+0x34);
   Need(c.source.Class({2,code})=="hkpMoppCode" && c.source.Class({2,mesh})=="hkpStorageExtendedMeshShape","Static root closure type mismatch");
   for(const auto object:{root,code,mesh})if(reachable.insert(object).second)remove(object,geometry_sizes.at(c.source.Class({2,object})),true);
   Need(U32(c.data,mesh+0xc4)==0 && U32(c.data,mesh+0x100)==0 && c.Pointer(mesh+0xc0,true)==UINT32_MAX && c.Pointer(mesh+0xfc,true)==UINT32_MAX,
       "Static triangle resource contains convex shape subparts/storage");
   const auto parts=U32(c.data,mesh+0xb8),storages=U32(c.data,mesh+0xf4);
   Need(parts && parts<=1024 && parts==storages,"Static triangle subpart/storage count mismatch");
   const auto part_array=c.Pointer(mesh+0xb4),storage_array=c.Pointer(mesh+0xf0);
   remove(part_array,uint64_t(parts)*0x60,false);remove(storage_array,uint64_t(storages)*4,false);
   for(uint32_t i=0;i<storages;++i){const auto storage=c.Pointer(storage_array+i*4);
    Need(c.source.Class({2,storage})=="hkpStorageExtendedMeshShapeMeshSubpartStorage","Static source storage class mismatch");
    if(reachable.insert(storage).second)remove(storage,0x60,true);
   }
   uint32_t native_mesh=0,native_root=0;
   if(auto existing=meshes.find(mesh);existing!=meshes.end()){
    Need(existing->second.bytes==body.native_shape_pack,"Shared original shape has ambiguous per-body native geometry/materials");
    native_mesh=existing->second.mesh_target;native_root=existing->second.root_target;
   }else{
    Pack native(body.native_shape_pack,false);const auto data=native.Data(2);
    const auto root_offset=U32(native.bytes,28);
    Need(U32(native.bytes,24)==2 && native.Class({2,0})=="hkpStorageExtendedMeshShape" &&
        native.Class({2,root_offset})==(accelerated?"hkpMoppBvTreeShape":"hkpStorageExtendedMeshShape"),"Static imported native root mismatch");
    Need(accelerated || root_offset==0,"Unaccelerated native root moved");
    if(accelerated)Need(native.Pointer({2,root_offset+0x34}).section==2 && native.Pointer({2,root_offset+0x34}).offset==0,
        "Imported MOPP child does not reference the native mesh");
    const auto globals=global_sources(native);native_mesh=c.output.Allocate(data.size());native_root=native_mesh+root_offset;Copy(c.output.data,native_mesh,data);
    size_t storage_classes=0,mopp_classes=0,code_classes=0;
    for(const auto& [key,name]:native.classes){const auto offset=uint32_t(key);
     uint32_t type=0,size=0;
     if(name=="hkpStorageExtendedMeshShape"){type=13;size=0x110;Need(offset==0,"Multiple imported native mesh roots");}
     else if(name=="hkpStorageExtendedMeshShapeMeshSubpartStorage"){type=14;size=0x68;++storage_classes;}
     else if(accelerated && name=="hkpMoppBvTreeShape"){type=0;size=0x40;++mopp_classes;Need(offset==root_offset,"Foreign imported MOPP root");}
     else if(accelerated && name=="hkpMoppCode"){type=1;size=0x30;++code_classes;Need(native.Pointer({2,root_offset+0x14}).offset==offset,"Foreign imported MOPP code");}
     else Fail("Unsupported imported native shape object");
     Need(uint32_t(key>>32)==2 && offset<=data.size() && size<=data.size()-offset,"Imported native shape object range");
     c.output.virtuals.push_back({native_mesh+offset,0,type});
    }
    Need(storage_classes==parts,"Imported native triangle storage closure differs");
    Need(mopp_classes==size_t(accelerated) && code_classes==size_t(accelerated),"Imported acceleration class closure differs");
    for(const auto& [key,to]:native.pointers){Need(uint32_t(key>>32)==2 && to.section==2,"Cross-section imported shape pointer");
     const auto from=uint32_t(key);c.output.Pointer(native_mesh+from,native_mesh+to.offset,globals.contains(from));}
    meshes.emplace(mesh,ImportedShape{body.native_shape_pack,native_mesh,native_root});
   }
   for(const auto object:{root,mesh}){
    const auto target=object==root?native_root:native_mesh;
    auto [it,inserted]=shape_targets.emplace(object,target);
    Need(inserted || it->second==target,"Shared original root has ambiguous native replacement");
   }
  }
  Need(reachable==geometry_objects,"Unreachable original static shape would be dropped");
  for(const auto& [key,name]:c.source.classes)if(metadata_types.contains(name))c.output.virtuals.push_back({uint32_t(key),0,metadata_types.at(name)});
  for(const auto& [key,to]:c.source.pointers){
   uint32_t from=uint32_t(key),target=to.offset;
   if(is_removed(from))continue; // Exact old object/part/pointer-array schemas only.
   for(const auto& [old,relocated]:bodies)if(from>=old && uint64_t(from)<uint64_t(old)+0x220){
    const auto field=from-old;Need(field==0x10 || field==0x74 || field==0x78,"Unsupported static body pointer field");
    from=relocated+(field==0x74?0x78:field==0x78?0x7c:field);break;
   }
   if(auto found=bodies.find(target);found!=bodies.end())target=found->second;
   else if(auto found=shape_targets.find(target);found!=shape_targets.end())target=found->second;
   else{
    for(const auto& [old,relocated]:bodies)Need(target<old || uint64_t(target)>=uint64_t(old)+0x220,"Interior static body reference unsupported");
    Need(!is_removed(target),"Active metadata references removed static shape interior/code/storage");
   }
   c.output.Pointer(from,target,old_globals.contains(uint32_t(key)));
  }
  auto pack=c.Finish(4);Pack after(pack,false);Need(after.Class({2,0})=="hkRootLevelContainer","Static resource root changed");
  ConvertedConvexBodyResource ready;for(const auto& [old,target]:bodies){Need(after.Class({2,target})=="hkpRigidBody","Static body class lost");ready.body_offsets.push_back(target);}
  ready.body_offset=ready.body_offsets.front();ready.pack=std::move(pack);result=std::move(ready);return true;
 }catch(const std::exception& exception){result={};error=exception.what();return false;}
}
bool TestStaticTriangleBodyResourceConversion(std::span<const uint8_t> original,std::string& error){
 try{
  ConvertedConvexBodyResource converted;if(!ConvertStaticTriangleBodyResource(original,converted,error))return false;
  Pack before(original,true),after(converted.pack,false);const auto old=before.Data(2),data=after.Data(2);
  Need(data.size()>old.size() && std::equal(old.begin(),old.end(),data.begin()),"Static original metadata/backing bytes changed");
  AuthoredStaticCollision geometry;std::string why;Need(ReconstructAuthoredStaticCollision(original,geometry,why),"Original static fixture reconstruction failed");
  Need(geometry.bodies.size()==2 && converted.body_offsets.size()==2,"Static floor fixture must preserve both bodies");
  std::set<uint32_t> native_globals;const auto& native_section=after.sections[2];
  for(uint32_t p=native_section.global;p+12<=native_section.virtuals;p+=12){const auto field=U32(after.bytes,native_section.start+p);if(field==UINT32_MAX)break;native_globals.insert(field);}
  std::map<uint32_t,uint32_t> relocated;
  for(size_t i=0;i<geometry.bodies.size();++i){const auto& body=geometry.bodies[i];const auto source=body.source_offset,target=converted.body_offsets[i];relocated.emplace(source,target);
   Need(after.Class({2,target})=="hkpRigidBody" && B8(data,target+0xe8)==5,"Native static body type/motion changed");
   Need(U32(old,source+0xc)==U32(data,target+0xc) && U32(old,source+0x2c)==U32(data,target+0x2c),"Native static body userdata/filter changed");
   Need(before.String(before.Pointer({2,source+0x74}))==after.String(after.Pointer({2,target+0x78})),"Native static body name changed");
   Need(U32(old,source+0x90)==U32(data,target+0x8c) && U32(old,source+0x94)==U32(data,target+0x90),"Native static body friction/restitution changed");
   Need(std::equal(old.begin()+source+0xf0,old.begin()+source+0x130,data.begin()+target+0xf0),"Static body local pose was transformed/changed");
   const auto properties=U32(old,source+0x7c);Need(properties==U32(data,target+0x80),"Static body property count changed");
   if(properties){const auto a=before.Pointer({2,source+0x78}),b=after.Pointer({2,target+0x7c});
    Need(a.section==2 && b.section==2 && a.offset==b.offset && uint64_t(a.offset)+uint64_t(properties)*16<=old.size(),"Static property ownership changed");}
   const auto shape=after.Pointer({2,target+0x10});Pack expected(body.native_shape_pack,false);const auto bytes=expected.Data(2);
   Need(shape.section==2 && after.Class(shape)=="hkpStorageExtendedMeshShape" && uint64_t(shape.offset)+bytes.size()<=data.size(),"Static shape graph was not imported as native storage mesh");
   Need(std::equal(bytes.begin(),bytes.end(),data.begin()+shape.offset),"Native geometry arrays/materials/welding/userdata changed during import");
   for(const auto& [key,name]:expected.classes)Need(after.Class({2,shape.offset+uint32_t(key)})==name,"Native imported shape finish class lost");
   for(const auto& [key,to]:expected.pointers){const auto actual=after.Pointer({2,shape.offset+uint32_t(key)});
    Need(actual.section==2 && actual.offset==shape.offset+to.offset,"Native shape local/global array relocation changed");}
   const auto& section=expected.sections[2];for(uint32_t p=section.global;p+12<=section.virtuals;p+=12){const auto field=U32(expected.bytes,section.start+p);if(field==UINT32_MAX)break;
    Need(native_globals.contains(shape.offset+field),"Native shape object-global fixup became local");}
   const auto source_root=before.Pointer({2,source+0x10}).offset;
   Need(!after.classes.contains(Pack::Key({2,source_root})),"Old MOPP remains an active native object");
  }
  uint32_t system=UINT32_MAX;
  for(const auto& [key,name]:before.classes)if(name=="hkpPhysicsSystem")system=uint32_t(key);
  Need(system!=UINT32_MAX && after.Class({2,system})=="hkpPhysicsSystem","Native physics system metadata lost");
  const auto array=before.Pointer({2,system+8});Need(after.Pointer({2,system+8}).offset==array.offset,"System array storage moved");
  for(uint32_t i=0;i<U32(old,system+0xc);++i){const auto body=before.Pointer({2,array.offset+i*4}).offset;
   Need(after.Pointer({2,array.offset+i*4}).offset==relocated.at(body),"Native system body order/ownership changed");}
  const auto rejected=[&](const Bytes& bad){ConvertedConvexBodyResource output;std::string rejected_error;
   Need(!ConvertStaticTriangleBodyResource(bad,output,rejected_error) && !rejected_error.empty() && output.pack.empty() && output.body_offsets.empty(),"Invalid static resource was accepted/partially published");};
  auto mutate_global=[&](Bytes& bytes,uint32_t from,uint32_t section,uint32_t to){const auto& s=before.sections[2];
   for(uint32_t p=s.global;p+12<=s.virtuals;p+=12){const auto at=s.start+p;if(U32(bytes,at)==UINT32_MAX)break;
    if(U32(bytes,at)==from){W32(bytes,at+4,section);W32(bytes,at+8,to);return;}}
   Fail("Static fixture lacks required original global fixup");};
  Bytes bad(original.begin(),original.end());bad[18]=0;rejected(bad);
  bad.assign(original.begin(),original.end());bad[before.sections[2].start+geometry.bodies.front().source_offset+0xe8]=2;rejected(bad);
  const auto root=before.Pointer({2,geometry.bodies.front().source_offset+0x10}).offset;
  const auto mesh=before.Pointer({2,root+0x34}).offset;
  bad.assign(original.begin(),original.end());W32(bad,before.sections[2].start+mesh+0xc4,1);rejected(bad);
  bad.assign(original.begin(),original.end());W32(bad,before.sections[2].start+mesh+0x100,1);rejected(bad);
  for(const uint32_t field:{24u,36u,48u}){bad.assign(original.begin(),original.end());W32(bad,before.sections[2].start+system+field,1);rejected(bad);}
  uint32_t physics=UINT32_MAX;for(const auto& [key,type]:before.classes)if(type=="hkpPhysicsData")physics=uint32_t(key);
  Need(physics!=UINT32_MAX,"Fixture lacks original PhysicsData");
  bad.assign(original.begin(),original.end());W32(bad,before.sections[2].start+physics+16,2);rejected(bad);
  bad.assign(original.begin(),original.end());W32(bad,before.sections[2].start+physics+8,1);rejected(bad);
  bad.assign(original.begin(),original.end());mutate_global(bad,array.offset,2,geometry.bodies.front().source_offset+4);rejected(bad);
  bad.assign(original.begin(),original.end());mutate_global(bad,24,0,160);rejected(bad);
  bad.assign(original.begin(),original.end());mutate_global(bad,geometry.bodies.back().source_offset+0x10,2,root);rejected(bad);
  bad.assign(original.begin(),original.end());mutate_global(bad,geometry.bodies.back().source_offset+0x10,2,root);
  WF(bad,before.sections[2].start+geometry.bodies.back().source_offset+0x90,geometry.bodies.front().friction+.125f);rejected(bad);
  bad.assign(original.begin(),original.end());const std::string_view name="hkpMoppCode";
  const auto text=std::search(bad.begin(),bad.end(),name.begin(),name.end());Need(text!=bad.end(),"Fixture lacks class-name evidence");*text='X';rejected(bad);
  const auto rejected_userdata=[&](const Bytes& bytes,bool acceleration,std::string_view expected){
   ConvertedConvexBodyResource output;std::string reason;
   Need(!ConvertStaticTriangleBodyResource(bytes,output,reason,acceleration) && output.pack.empty() && output.body_offsets.empty() && reason==expected,
       "Unsafe active root userdata was accepted or lost its ownership diagnostic");
  };
  constexpr uint32_t userdata=0x12345678;
  bad.assign(original.begin(),original.end());W32(bad,before.sections[2].start+root+8,userdata);
  rejected_userdata(bad,true,"Native static component ownership rejected: active MOPP root has nonzero authored userdata");
  ConvertedConvexBodyResource inactive_mopp;Need(ConvertStaticTriangleBodyResource(bad,inactive_mopp,why,false),"Inactive authored MOPP userdata was incorrectly rejected");
  Pack inactive_mopp_pack(inactive_mopp.pack,false);const auto inactive_mopp_data=inactive_mopp_pack.Data(2);
  Need(U32(inactive_mopp_data,root+8)==userdata && U32(inactive_mopp_data,inactive_mopp_pack.Pointer({2,inactive_mopp.body_offsets.front()+0x10}).offset+8)==0,
       "Inactive MOPP userdata was erased or copied onto active storage root");
  bad.assign(original.begin(),original.end());W32(bad,before.sections[2].start+mesh+8,userdata);
  rejected_userdata(bad,false,"Native static component ownership rejected: active storage mesh root has nonzero authored userdata");
  ConvertedConvexBodyResource inactive_mesh;Need(ConvertStaticTriangleBodyResource(bad,inactive_mesh,why,true),"Inactive authored mesh userdata was incorrectly rejected");
  Pack inactive_mesh_pack(inactive_mesh.pack,false);const auto inactive_mesh_data=inactive_mesh_pack.Data(2);
  const auto active_mopp=inactive_mesh_pack.Pointer({2,inactive_mesh.body_offsets.front()+0x10}).offset;
  const auto inactive_child=inactive_mesh_pack.Pointer({2,active_mopp+0x34}).offset;
  Need(U32(inactive_mesh_data,mesh+8)==userdata && U32(inactive_mesh_data,inactive_child+8)==userdata && U32(inactive_mesh_data,active_mopp+8)==0,
       "Inactive mesh userdata was erased or copied onto active MOPP root");
  // A body-offset sort must never silently reorder the authored system array.
  bad.assign(original.begin(),original.end());mutate_global(bad,array.offset,2,geometry.bodies.back().source_offset);
  mutate_global(bad,array.offset+4,2,geometry.bodies.front().source_offset);
  ConvertedConvexBodyResource reversed;Need(ConvertStaticTriangleBodyResource(bad,reversed,why),"Valid reversed system order was rejected");
  Pack reverse(reversed.pack,false);Need(reverse.Pointer({2,array.offset}).offset==reversed.body_offsets.back() &&
    reverse.Pointer({2,array.offset+4}).offset==reversed.body_offsets.front(),"Authored reversed system body order lost");
  ConvertedConvexBodyResource explicit_default;
  Need(ConvertStaticTriangleBodyResource(original,explicit_default,why,false) && explicit_default.pack==converted.pack &&
       explicit_default.body_offsets==converted.body_offsets,"Explicit false changed the default static resource bytes");
  auto accelerated_geometry=geometry;Need(AccelerateAuthoredStaticCollision(accelerated_geometry,why),"Fixture original MOPP reconstruction failed");
  ConvertedConvexBodyResource accelerated;
  Need(ConvertStaticTriangleBodyResource(original,accelerated,why,true),"Accelerated complete static resource rejected");
  Pack tree(accelerated.pack,false);const auto tree_data=tree.Data(2);
  Need(std::equal(old.begin(),old.end(),tree_data.begin()) && accelerated.body_offsets==converted.body_offsets,
       "Acceleration changed metadata/body placement");
  for(size_t i=0;i<accelerated_geometry.bodies.size();++i){const auto& body=accelerated_geometry.bodies[i];
   Pack expected(body.native_shape_pack,false);const auto expected_data=expected.Data(2);const auto expected_root=U32(expected.bytes,28);
   const auto root=tree.Pointer({2,accelerated.body_offsets[i]+0x10});
   Need(root.section==2 && tree.Class(root)=="hkpMoppBvTreeShape","Accelerated body does not point at imported MOPP root");
   const auto mesh=tree.Pointer({2,root.offset+0x34}),code=tree.Pointer({2,root.offset+0x14});
   Need(mesh.section==2 && code.section==2 && tree.Class(mesh)=="hkpStorageExtendedMeshShape" && tree.Class(code)=="hkpMoppCode" &&
        root.offset==mesh.offset+expected_root && expected_root>0,"Accelerated mesh/root relocation collapsed");
   Need(uint64_t(mesh.offset)+expected_data.size()<=tree_data.size() &&
        std::equal(expected_data.begin(),expected_data.end(),tree_data.begin()+mesh.offset),"Accelerated native graph/geometry bytes changed");
   for(const auto& [key,name]:expected.classes)Need(tree.Class({2,mesh.offset+uint32_t(key)})==name,"Accelerated finish class lost");
   for(const auto& [key,to]:expected.pointers){const auto actual=tree.Pointer({2,mesh.offset+uint32_t(key)});
    Need(actual.section==2 && actual.offset==mesh.offset+to.offset,"Accelerated local/global fixup graph differs");}
   Need(U32(tree_data,code.offset+0x24)==body.original_mopp_code.size(),"Original MOPP byte count changed");
   const auto program=tree.Pointer({2,code.offset+0x20});
   Need(program.section==2 && uint64_t(program.offset)+body.original_mopp_code.size()<=tree_data.size() &&
       std::equal(body.original_mopp_code.begin(),body.original_mopp_code.end(),tree_data.begin()+program.offset),"Original MOPP program changed");
   for(unsigned lane=0;lane<4;++lane)Need(U32(tree_data,code.offset+0x10+lane*4)==std::bit_cast<uint32_t>(body.original_mopp_info[lane]),
       "Original MOPP quantization changed");
   Need(U32(tree_data,root.offset+8)==body.original_mopp_user_data && B8(tree_data,root.offset+0x10)==body.original_mopp_type &&
        B8(tree_data,code.offset+0x2c)==body.original_mopp_build_type,"Original MOPP userdata/type/build changed");
  }
  error.clear();return true;
 }catch(const std::exception& exception){error=exception.what();return false;}
}
bool TestConvexBodiesResourceConversion(std::span<const uint8_t> original,std::string& error){
 try{
  ConvertedConvexBodyResource converted;
  if(!ConvertConvexBodiesResource(original,converted,error))return false;
  Pack before(original,true),after(converted.pack,false);const auto old=before.Data(2),ready=after.Data(2);
  std::vector<uint32_t> bodies;
  for(const auto& [key,name]:before.classes)if(name=="hkpRigidBody")bodies.push_back(uint32_t(key));
  Need(bodies.size()>1 && converted.body_offsets.size()==bodies.size(),"Multi-body fixture lost an authored body");
  std::map<uint32_t,uint32_t> relocations;
  for(size_t i=0;i<bodies.size();++i){const auto source=bodies[i],target=converted.body_offsets[i];relocations.emplace(source,target);
   Need(after.Class({2,target})=="hkpRigidBody","Converted body has incorrect native type");
   Need(U32(old,source+0xc)==U32(ready,target+0xc) && U32(old,source+0x2c)==U32(ready,target+0x2c),"Body userdata/filter changed");
   Need(before.String(before.Pointer({2,source+0x74}))==after.String(after.Pointer({2,target+0x78})),"Authored body name changed");
   Need(U32(old,source+0x90)==U32(ready,target+0x8c) && U32(old,source+0x94)==U32(ready,target+0x90),"Authored body material changed");
   Need(B8(old,source+0xe8)==B8(ready,target+0xe8),"Authored body motion type changed");
   const auto shape=before.Pointer({2,source+0x10}).offset,native=after.Pointer({2,target+0x10}).offset;
   Need(after.Class({2,native})=="hkpConvexVerticesShape" && U32(old,shape+8)==U32(ready,native+8) &&
     U32(old,shape+0x10)==U32(ready,native+0x10),"Convex type/userdata/radius changed");
   for(auto fields: {std::array<uint32_t,3>{0x40,0x44,48},std::array<uint32_t,3>{0x54,0x58,16}}){
    const auto count=U32(old,shape+fields[1]);Need(count==U32(ready,native+fields[1]),"Convex vector/plane count changed");
    if(count){const auto from=before.Pointer({2,shape+fields[0]}).offset,to=after.Pointer({2,native+fields[0]}).offset;const auto bytes=size_t(count)*fields[2];
     Need(uint64_t(from)+bytes<=old.size() && uint64_t(to)+bytes<=ready.size() &&
       std::equal(old.begin()+from,old.begin()+from+bytes,ready.begin()+to),"Convex vectors/planes changed during relocation");}
   }
  }
  // System-array references retain their order and target every relocated body.
  size_t references=0;
  for(const auto& [key,to]:before.pointers)if(to.section==2 && relocations.contains(to.offset)){
   const auto location=Location{uint32_t(key>>32),uint32_t(key)};
   Need(location.section==2 && after.Pointer(location).offset==relocations.at(to.offset),"System body reference was lost/reordered");++references;
  }
  Need(references>=bodies.size(),"Fixture does not cover system ownership of every body");
  auto corrupt=std::vector<uint8_t>(original.begin(),original.end());corrupt[18]=0;ConvertedConvexBodyResource rejected;std::string why;
  Need(!ConvertConvexBodiesResource(corrupt,rejected,why) && rejected.pack.empty() && rejected.body_offsets.empty(),"Wrong source ABI accepted");
  error.clear();return true;
 }catch(const std::exception& exception){error=exception.what();return false;}
}
bool TestBoxBodyResourceConversion(std::span<const uint8_t> original,std::string& error){
 try {
  ConvertedConvexBodyResource a,b;
  if(!ConvertBoxBodyResource(original,a,error) || !ConvertBoxBodyResource(original,b,error))return false;
  Need(a.pack==b.pack && a.body_offset==b.body_offset,"Box body conversion is not deterministic");
  Pack before(original,true),after(a.pack,false);const auto old=before.Data(2),now=after.Data(2);const auto body=a.body_offset;
  Need(before.Class({2,body})=="hkpRigidBody" && after.Class({2,body})=="hkpRigidBody","Box body class changed");
  Need(U32(old,body+8)==U32(now,body+8) && U32(old,body+0x28)==U32(now,body+0x28),"Authored body userdata/filter changed");
  Need(U32(old,body+0x90)==U32(now,body+0x8c) && U32(old,body+0x94)==U32(now,body+0x90),"Authored box material changed during native offset migration");
  Need(std::equal(old.begin()+body+0xf0,old.begin()+body+0x130,now.begin()+body+0xf0),"Authored box body transform changed");
  auto old_shape=before.Pointer({2,body+0x10}).offset,new_shape=after.Pointer({2,body+0x10}).offset;unsigned wrappers=0;
  for(;;){
   const auto type=before.Class({2,old_shape});Need(type==after.Class({2,new_shape}),"Box shape graph class changed");
   Need(U32(old,old_shape+8)==U32(now,new_shape+8) && U32(old,old_shape+0x10)==U32(now,new_shape+0x10),"Authored shape userData/radius changed");
   Need(std::equal(old.begin()+old_shape+0x20,old.begin()+old_shape+0x30,now.begin()+new_shape+0x20),"Authored translation/box half-extents changed");
   if(type=="hkpBoxShape")break;
   Need(type=="hkpConvexTranslateShape" && ++wrappers<=32,"Unexpected native boxed shape closure");
   old_shape=before.Pointer({2,old_shape+0x18}).offset;new_shape=after.Pointer({2,new_shape+0x18}).offset;
  }
  const auto source_at=before.sections[2].start;
  for(const auto [at,value]:{std::pair{source_at+old_shape+0x20,0xbf800000u},std::pair{source_at+old_shape+0x10,0xbf800000u},std::pair{source_at+old_shape+0x24,0x7fc00000u}}){
   Bytes broken(original.begin(),original.end());W32(broken,at,value);ConvertedConvexBodyResource rejected;std::string why;
   Need(!ConvertBoxBodyResource(broken,rejected,why) && rejected.pack.empty() && !why.empty(),"Invalid authored box data accepted");
  }
  // Corrupt an actual wrapper child fixup into a self-reference. This must
  // reject, not recurse indefinitely or silently omit the source shape.
  const auto root=before.Pointer({2,body+0x10}).offset;
  if(before.Class({2,root})=="hkpConvexTranslateShape"){
   Bytes cyclic(original.begin(),original.end());bool changed=false;const auto& s=before.sections[2];
   for(uint32_t p=s.local;p+8<=s.global;p+=8)if(U32(cyclic,s.start+p)==root+0x18){W32(cyclic,s.start+p+4,root);changed=true;break;}
   for(uint32_t p=s.global;p+12<=s.virtuals && !changed;p+=12)if(U32(cyclic,s.start+p)==root+0x18){W32(cyclic,s.start+p+8,root);changed=true;break;}
   Need(changed,"Translated box child fixup missing from fixture");ConvertedConvexBodyResource rejected;std::string why;
   Need(!ConvertBoxBodyResource(cyclic,rejected,why) && rejected.pack.empty() && !why.empty(),"Cyclic boxed shape graph accepted");
  }
  error.clear();return true;
 }catch(const std::exception& exception){error=exception.what();return false;}
}
} // namespace em2::em1
bool em2::em1::AdaptJigsawHavokBlocks(nif::Nif& model,std::vector<JigsawAggregateInfo>& info,std::string& error,std::vector<nif::Bytes>* converted_packs) {
 info.clear();error.clear();try{
  Need(model.big,"Jigsaw source block byte order must remain big endian");std::vector<std::pair<size_t,Bytes>> changes;std::vector<JigsawAggregateInfo> prepared;std::vector<Bytes> packs;
  for(size_t i=0;i<model.blocks.size();++i)if(model.types[model.blocks[i].type]=="JPSJigsawNode"){
   const auto original=model.blocks[i].Data();const auto span=Embedded(original,uint32_t(model.blocks.size()));ConvertedJigsawAggregate converted;
   const auto bindings=ReadJigsawMeshBindings(model,original.subspan(span.blob_at+span.blob_size),span.pieces);
   Converter converter(original.subspan(span.blob_at,span.blob_size),false,&bindings);converter.Convert();converted.pack=converter.Finish();converted.info=std::move(converter.info);
   Pack check(converted.pack,false);Need(check.Class({2,0})=="hkpMoppBvTreeShape","Native bound Jigsaw root mismatch");
   Bytes block;block.insert(block.end(),original.begin(),original.begin()+span.length_at);nif::Number(block,uint32_t(converted.pack.size()),true);
   block.insert(block.end(),converted.pack.begin(),converted.pack.end());block.insert(block.end(),original.begin()+span.blob_at+span.blob_size,original.end());
   Need(std::equal(original.begin(),original.begin()+span.length_at,block.begin()),"Jigsaw prefix changed");
   changes.emplace_back(i,std::move(block));prepared.push_back(std::move(converted.info));if(converted_packs)packs.push_back(std::move(converted.pack));
  }
  for(auto& [index,bytes]:changes)model.blocks[index].replacement=std::move(bytes);info=std::move(prepared);if(converted_packs)*converted_packs=std::move(packs);return true;
 }catch(const std::exception& exception){info.clear();error=exception.what();return false;}
}
bool em2::em1::TestJigsawAggregateConversion(std::span<const uint8_t> original,std::string& error) {
 try{ConvertedJigsawAggregate a,b;if(!ConvertJigsawAggregate(original,a,error) || !ConvertJigsawAggregate(original,b,error))return false;
  Need(a.pack==b.pack && !a.info.triangles.empty() && a.info.convex_shapes && !a.info.convex_vertices.empty(),"Jigsaw deterministic conversion/geometry proof");
  Need(!a.info.native_load_proven && !a.info.mopp_key_parity_proven,"CPU conversion must not claim native proof");
  Pack before(original,true),after(a.pack,false);const auto old=before.Data(2),now=after.Data(2);
  const auto oldcode=before.Pointer({2,0x14}).offset,newcode=after.Pointer({2,0x14}).offset;
  const auto oldbytes=before.Pointer({2,oldcode+0x20}).offset,newbytes=after.Pointer({2,newcode+0x20}).offset,count=U32(old,oldcode+0x24);
  Need(count==U32(now,newcode+0x24) && std::equal(old.begin()+oldbytes,old.begin()+oldbytes+count,now.begin()+newbytes),"Jigsaw MOPP program changed");
  Need(std::equal(old.begin()+oldcode+0x10,old.begin()+oldcode+0x20,now.begin()+newcode+0x10),"Jigsaw MOPP quantization changed");
  auto broken=Bytes(original.begin(),original.end());broken[18]=0;ConvertedJigsawAggregate rejected;std::string reason;
  Need(!ConvertJigsawAggregate(broken,rejected,reason) && rejected.pack.empty() && !reason.empty(),"Jigsaw wrong source ABI accepted");return true;
 }catch(const std::exception& exception){error=exception.what();return false;}
}
