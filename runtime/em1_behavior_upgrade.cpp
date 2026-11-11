#include "em1_behavior_upgrade.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <compare>
#include <set>
#include <stdexcept>
#include <tuple>
namespace em2::em1 { namespace {
using Bytes=std::vector<uint8_t>;
void Need(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
uint32_t U(std::span<const uint8_t> b,size_t at){Need(at<=b.size()&&4<=b.size()-at,"Behavior word outside resource");return(uint32_t(b[at])<<24)|(uint32_t(b[at+1])<<16)|(uint32_t(b[at+2])<<8)|b[at+3];}
uint16_t H(std::span<const uint8_t>b,size_t at){Need(at<=b.size()&&2<=b.size()-at,"Behavior short outside resource");return uint16_t(b[at]<<8)|b[at+1];}
void W(Bytes& b,size_t at,uint32_t v){Need(at<=b.size()&&4<=b.size()-at,"Behavior output word outside resource");for(unsigned i=0;i<4;++i)b[at+i]=uint8_t(v>>(24-i*8));}
struct At {uint32_t section=0,offset=0;auto operator<=>(const At&)const=default;};
struct Section{uint32_t start,local,global,virtuals,exports,imports,end;};
struct Pack {
 std::span<const uint8_t> bytes;std::array<Section,3> sections;std::map<At,At> pointers;std::map<At,std::string> objects;std::map<std::string,NativeBehaviorClass> schemas;
 Pack(std::span<const uint8_t>b,bool original=true):bytes(b){
  Need(b.size()>=208&&U(b,0)==0x57e0e057&&U(b,4)==0x10c0c010,"Not a binary Havok behavior pack");
  Need(U(b,12)==(original?7:9)&&U(b,16)==(original?0x04000101:0x04000001)&&U(b,20)==3,"Behavior source must be original BE32 Havok7 layout");
  Need(std::memcmp(b.data()+40,original?"Havok-7.0.0-r1":"hk_2011.2.0-r1",14)==0&&b[54]==0,"Behavior source version mismatch");
  for(unsigned s=0;s<3;++s){const auto p=84+s*48;auto& r=sections[s];r={U(b,p),U(b,p+4),U(b,p+8),U(b,p+12),U(b,p+16),U(b,p+20),U(b,p+24)};
   Need(r.local<=r.global&&r.global<=r.virtuals&&r.virtuals<=r.exports&&r.exports<=r.imports&&r.imports<=r.end&&uint64_t(r.start)+r.end<=b.size(),"Behavior section bounds/order invalid");
   Need(r.exports==r.imports&&r.imports==r.end,"Behavior imports/exports are unsupported");
  }
  auto ptr=[&](At from,At to){Need(from.section<3&&to.section<3&&from.offset+4<=sections[from.section].local&&to.offset<sections[to.section].local,"Behavior fixup outside object data");Need(pointers.emplace(from,to).second,"Duplicate behavior pointer fixup");};
  for(unsigned s=0;s<3;++s){const auto&r=sections[s];
   for(uint32_t p=r.local;p+8<=r.global;p+=8){auto f=U(b,r.start+p);if(f==UINT32_MAX)break;ptr({s,f},{s,U(b,r.start+p+4)});}
   for(uint32_t p=r.global;p+12<=r.virtuals;p+=12){auto f=U(b,r.start+p);if(f==UINT32_MAX)break;ptr({s,f},{U(b,r.start+p+4),U(b,r.start+p+8)});}
   for(uint32_t p=r.virtuals;p+12<=r.exports;p+=12){auto f=U(b,r.start+p);if(f==UINT32_MAX)break;Need(f<r.local,"Behavior object fixup outside data");At name{U(b,r.start+p+4),U(b,r.start+p+8)};Need(objects.emplace(At{s,f},Text(name)).second,"Duplicate behavior class fixup");}
  }
  for(const auto&[at,name]:objects)if(at.section==2)Need(name=="hkRootLevelContainer"||name.starts_with("hkb"),"Resource is not an audited HKB behavior/project/character closure: "+name);
  for(const auto&[at,name]:objects)if(at.section==1&&name=="hkClass"){
   NativeBehaviorClass c;c.name=Text(Pointer(at));c.parent=ClassName(Pointer(Add(at,4)));c.size=Word(at,8);c.version=Word(at,44);c.flags=Word(at,40);
   Need(!c.name.empty()&&c.size&&c.size<=65536,"Invalid source behavior class schema");
   auto members=Pointer(Add(at,24));const auto count=Word(at,28);Need(count<=1024,"Behavior reflected member budget exceeded");
   auto defaults=Pointer(Add(at,32));
   for(uint32_t i=0;i<count;++i){auto m=Add(members,i*24);auto raw=Data(m,24);BehaviorMember v;v.name=Text(Pointer(m));v.klass=ClassName(Pointer(Add(m,4)));v.type=raw[12];v.subtype=raw[13];v.array_size=H(raw,14);v.flags=H(raw,16);v.offset=H(raw,18);
    auto en=Pointer(Add(m,8));if(en!=At{}){auto items=Pointer(Add(en,4));auto n=Word(en,8);Need(n<=2048,"Behavior enum budget exceeded");for(uint32_t j=0;j<n;++j)v.enumeration.push_back({int32_t(Word(items,j*8)),Text(Pointer(Add(items,j*8+4)))});}
    Need(!v.name.empty()&&v.offset<c.size,"Invalid reflected behavior member");c.members.push_back(std::move(v));
   }
   Need(schemas.emplace(c.name,std::move(c)).second,"Duplicate source behavior class schema");
  }
  Need(U(b,24)==2&&Class({2,U(b,28)})=="hkRootLevelContainer","Behavior resource root is not a level container");
 }
 static At Add(At a,uint32_t n){Need(a.offset<=UINT32_MAX-n,"Behavior pointer overflow");a.offset+=n;return a;}
 std::span<const uint8_t> Data(At a,size_t n)const{Need(a.section<3&&a.offset<=sections[a.section].local&&n<=sections[a.section].local-a.offset,"Behavior object outside source section");return bytes.subspan(sections[a.section].start+a.offset,n);}
 uint32_t Word(At a,uint32_t n=0)const{return U(Data(Add(a,n),4),0);}
 At Pointer(At a)const{if(a==At{})return{};auto i=pointers.find(a);if(i!=pointers.end())return i->second;Need(Word(a)==0,"Unfixed non-null behavior pointer");return{};}
 std::string Text(At a)const{if(a==At{})return{};auto b=Data(a,1);const auto&r=sections[a.section];auto begin=bytes.begin()+r.start+a.offset,end=bytes.begin()+r.start+r.local;auto stop=std::find(begin,end,0);Need(stop!=end&&stop-begin<=4096,"Unterminated/oversized behavior string");return{begin,stop};}
 std::string ClassName(At a)const{return a==At{}?std::string{}:Text(Pointer(a));}
 std::string Class(At a)const{auto i=objects.find(a);return i==objects.end()?std::string{}:i->second;}
};
uint32_t Primitive(uint8_t type){switch(type){case 1:case 2:case 3:case 4:return 1;case 5:case 6:case 32:return 2;case 7:case 8:case 11:case 20:case 21:case 29:case 30:case 33:return 4;case 9:case 10:return 8;case 12:case 13:return 16;case 14:case 15:case 16:return 48;case 17:case 18:return 64;case 22:return 12;case 26:return 8;case 28:return 8;default:return 0;}}
struct Converter {
 Pack source;const NativeBehaviorSchemas& native;Bytes data;std::map<uint32_t,uint32_t> pointers;std::map<uint32_t,std::string> finish;std::map<At,std::pair<std::string,uint32_t>> relocated;std::map<At,uint32_t> strings;std::vector<BehaviorDependency> dependencies;uint32_t migrations=0;std::set<At> retired_runtime_objects;
 Converter(std::span<const uint8_t>b,const NativeBehaviorSchemas&n):source(b),native(n){}
 const NativeBehaviorClass& Schema(std::string_view name,bool target)const{const auto& all=target?native.classes:source.schemas;auto i=all.find(std::string(name));Need(i!=all.end(),"Missing "+std::string(target?"native":"source")+" behavior reflection: "+std::string(name));Need(i->second.size&&i->second.size<=65536,"Invalid native behavior class extent");return i->second;}
 std::map<std::string,BehaviorMember> Fields(std::string name,bool target,unsigned depth=0)const{Need(depth<64,"Behavior inheritance cycle/depth");const auto& c=Schema(name,target);auto out=c.parent.empty()?std::map<std::string,BehaviorMember>{}:Fields(c.parent,target,depth+1);for(const auto&m:c.members)Need(out.emplace(m.name,m).second,"Conflicting inherited behavior member: "+name+"."+m.name);return out;}
 uint32_t Size(const BehaviorMember&m,bool target)const{uint32_t n=m.type==25?Schema(m.klass,target).size:Primitive(m.type==24||m.type==31?m.subtype:m.type);Need(n,"Unsupported behavior member type "+std::to_string(m.type));Need(!m.array_size||n<=65536/m.array_size,"Behavior inline array extent exceeds budget");return n*(m.array_size?m.array_size:1);}
 uint32_t Allocate(uint32_t n){Need(n<=64*1024*1024&&data.size()+15ull+n<=64*1024*1024,"Converted behavior allocation budget exceeded");while(data.size()%16)data.push_back(0);auto off=uint32_t(data.size());data.resize(data.size()+n);return off;}
 void Link(uint32_t at,uint32_t to){Need(uint64_t(at)+4<=data.size()&&to<data.size(),"Converted behavior pointer invalid");Need(pointers.emplace(at,to).second,"Duplicate converted behavior pointer");}
 void EmptyArrays(const std::string&klass,uint32_t dst){for(const auto&[name,m]:Fields(klass,true))if(m.type==22)W(data,dst+m.offset+8,0x80000000);}
 uint32_t NewObject(const std::string&klass){const auto&c=Schema(klass,true);Need(c.signature_verified,"Native behavior class signature is unverified: "+klass);auto dst=Allocate(c.size);EmptyArrays(klass,dst);finish.emplace(dst,klass);return dst;}
 uint32_t String(At at,const std::string& owner,const BehaviorMember&m,uint32_t index){if(at==At{})return UINT32_MAX;auto value=source.Text(at);auto found=strings.find(at);uint32_t dst;if(found!=strings.end())dst=found->second;else{dst=Allocate(uint32_t(value.size()+1));std::copy(value.begin(),value.end(),data.begin()+dst);strings.emplace(at,dst);}if(value.find(".hk")!=value.npos||m.name.ends_with("Path"))dependencies.push_back({owner,m.name,value,index,dst});return dst;}
 bool IsA(std::string actual,std::string declared)const{if(declared.empty())return false;for(unsigned i=0;!actual.empty()&&i<64;++i){if(actual==declared)return true;actual=Schema(actual,false).parent;}return false;}
 uint32_t Object(At at,const std::string&declared){Need(at.section==2,"Behavior object points outside data closure");auto type=source.Class(at);if(type.empty())type=declared;Need(!type.empty()&&(declared.empty()||IsA(type,declared)),"Behavior pointer class differs from reflected declaration");auto found=relocated.find(at);if(found!=relocated.end()){Need(found->second.first==type,"Shared behavior object has conflicting classes");return found->second.second;}auto dst=NewObject(type);Need(relocated.size()<100000,"Behavior object-count budget exceeded");relocated.emplace(at,std::pair{type,dst});Value(at,type,dst);return dst;}
 void Member(At at,const BehaviorMember& old,const BehaviorMember& now,uint32_t dst,const std::string&owner,uint32_t index=UINT32_MAX){
  Need(old.type==now.type&&old.array_size==now.array_size,"Behavior field requires semantic migration: "+owner+"."+now.name);
  if(old.array_size){auto a=old,b=now;a.array_size=b.array_size=0;const auto os=Size(a,false),ns=Size(b,true);for(uint32_t i=0;i<old.array_size;++i)Member(Pack::Add(at,i*os),a,b,dst+i*ns,owner,i);return;}
  if(old.type==25){Need(old.klass==now.klass,"Behavior inline struct type changed");Value(at,old.klass,dst);return;}
  if(old.type==20){auto p=source.Pointer(at);if(p==At{})return;Need(!old.klass.empty()&&old.klass==now.klass,"Unreflected behavior pointer cannot migrate");Link(dst,Object(p,old.klass));return;}
  if(old.type==33||old.type==29){auto p=source.Pointer(at);if(p==At{})return;Link(dst,String(p,owner,now,index));return;}
  if(old.type==22||old.type==26){Need(old.subtype==now.subtype&&old.klass==now.klass,"Behavior array element type changed");auto n=source.Word(at,4);Need(n<=1000000,"Behavior array element budget exceeded");W(data,dst+4,n);if(old.type==22)W(data,dst+8,n|0x80000000);auto p=source.Pointer(at);Need((n==0)||(p!=At{}),"Behavior nonempty array lacks storage");if(!n)return;BehaviorMember a=old,b=now;a.type=old.subtype;b.type=now.subtype;a.array_size=b.array_size=0;auto os=Size(a,false),ns=Size(b,true);source.Data(p,size_t(n)*os);Need(uint64_t(n)*ns<=64*1024*1024,"Behavior array byte budget exceeded");auto block=Allocate(n*ns);Link(dst,block);for(uint32_t i=0;i<n;++i)Member(Pack::Add(p,i*os),a,b,block+i*ns,owner,i);return;}
  if(old.type==24||old.type==31){
   Need(old.subtype==now.subtype,"Behavior enum width changed");const auto width=Size(old,false);Need(width<=4&&!old.enumeration.empty()&&!now.enumeration.empty(),"Behavior enum lacks verified source/native values");auto raw=source.Data(at,width);uint32_t bits=0;for(auto byte:raw)bits=(bits<<8)|byte;
   int32_t numeric=int32_t(bits);if((old.subtype==3||old.subtype==5)&&width<4&&(bits&(1u<<(width*8-1))))numeric=int32_t(bits|(~0u<<(width*8)));
   auto target=[&](std::string name)->std::optional<int32_t>{
    if(owner=="hkbStateMachine"&&old.name=="startStateMode"&&name=="START_STATE_MODE_SET_ON_ACTIVATE")name="START_STATE_MODE_DEFAULT";
    for(const auto&e:now.enumeration)if(e.name==name)return e.value;return{};
   };
   std::optional<int32_t> mapped;
   const uint32_t width_mask=width==4?UINT32_MAX:((1u<<(width*8))-1);
   for(const auto&e:old.enumeration)if((uint32_t(e.value)&width_mask)==bits){auto value=target(e.name);if(value){Need(!mapped||*mapped==*value,"Ambiguous native enum aliases");mapped=value;}}
   if(old.type==31&&!mapped){uint32_t remaining=bits,value=0;for(const auto&e:old.enumeration){const auto bit=uint32_t(e.value);if(!bit||(bit&(bit-1))||!(remaining&bit))continue;auto to=target(e.name);Need(to.has_value(),"Original behavior flag has no native symbolic equivalent: "+owner+"."+old.name+"."+e.name);value|=uint32_t(*to);remaining&=~bit;}Need(!remaining,"Original behavior flags contain unmapped bits");mapped=int32_t(value);}
   Need(mapped.has_value(),"Original behavior enum value has no native semantic equivalent: "+owner+"."+old.name+"="+std::to_string(numeric));
   for(uint32_t i=0;i<width;++i)data[dst+i]=uint8_t(uint32_t(*mapped)>>(8*(width-1-i)));if(uint32_t(*mapped)!=bits)++migrations;return;
  }
  const auto n=Size(old,false);Need(n==Size(now,true)&&old.type!=21&&old.type!=28,"Unsupported behavior scalar/pointer representation");const auto raw=source.Data(at,n);std::copy(raw.begin(),raw.end(),data.begin()+dst);
 }
 void Defaults(const std::string&klass,uint32_t dst,unsigned depth=0){
  Need(depth<32,"Native behavior default struct recursion exceeds budget");const auto&c=Schema(klass,true);Need(uint64_t(dst)+c.size<=data.size(),"Native behavior default struct extent invalid");EmptyArrays(klass,dst);
  for(const auto&[name,m]:Fields(klass,true)){if(m.flags&0x400)continue;const auto n=Size(m,true);Need(uint64_t(m.offset)+n<=c.size,"Native default field exceeds class extent");
   if(m.default_bytes){Need(m.default_bytes->empty()||m.default_bytes->size()==n,"Native nested default byte extent differs");if(!m.default_bytes->empty())std::copy(m.default_bytes->begin(),m.default_bytes->end(),data.begin()+dst+m.offset);}
   else if(m.type==25)Defaults(m.klass,dst+m.offset,depth+1);
   else Need(m.type==20||m.type==22||m.type==33,"Native new struct lacks a verified scalar default: "+klass+"."+name);
  }
 }
 void Value(At at,const std::string&klass,uint32_t dst);
 Bytes Finish(uint32_t root){
  Bytes names;std::map<std::string,uint32_t> nameOffsets;for(const auto&[at,name]:finish)if(!nameOffsets.contains(name)){const auto&c=Schema(name,true);const auto start=names.size();names.resize(start+5);W(names,start,c.signature);names[start+4]=9;nameOffsets.emplace(name,uint32_t(start+5));names.insert(names.end(),name.begin(),name.end());names.push_back(0);}while(names.size()%16)names.push_back(0xff);
  while(data.size()%16)data.push_back(0);const auto local=uint32_t(data.size());for(const auto&[at,to]:pointers){auto p=data.size();data.resize(p+8);W(data,p,at);W(data,p+4,to);}while(data.size()%16)data.push_back(0xff);const auto global=uint32_t(data.size()),virtuals=global;
  for(const auto&[at,name]:finish){auto p=data.size();data.resize(p+12);W(data,p,at);W(data,p+4,0);W(data,p+8,nameOffsets.at(name));}while(data.size()%16)data.push_back(0xff);const auto end=uint32_t(data.size());
  Bytes out(208,0);W(out,0,0x57e0e057);W(out,4,0x10c0c010);W(out,8,U(source.bytes,8));W(out,12,9);W(out,16,0x04000001);W(out,20,3);W(out,24,2);W(out,28,root);W(out,32,0);W(out,36,nameOffsets.at("hkRootLevelContainer"));std::memcpy(out.data()+40,"hk_2011.2.0-r1",14);out[55]=0xff;W(out,60,UINT32_MAX);
  const auto dataStart=208+uint32_t(names.size());auto section=[&](unsigned index,const char*name,std::array<uint32_t,7>v){auto p=64+index*48;std::memcpy(out.data()+p,name,std::strlen(name));for(unsigned i=0;i<7;++i)W(out,p+20+i*4,v[i]);};const auto n=uint32_t(names.size());section(0,"__classnames__",{208,n,n,n,n,n,n});section(1,"__types__",{dataStart,0,0,0,0,0,0});section(2,"__data__",{dataStart,local,global,virtuals,end,end,end});out.insert(out.end(),names.begin(),names.end());out.insert(out.end(),data.begin(),data.end());return out;
 }
};
void Converter::Value(At at,const std::string& klass,uint32_t dst){
 const auto& oldSchema=Schema(klass,false);const auto& newSchema=Schema(klass,true);source.Data(at,oldSchema.size);Need(uint64_t(dst)+newSchema.size<=data.size(),"Behavior destination struct extent invalid");
 auto old=Fields(klass,false),now=Fields(klass,true);std::set<std::string> consumed,generated;
 auto count=[&](const BehaviorMember&m){Need(m.type==22,"Behavior migration requires an authored array");return source.Word(at,m.offset+4);};
 if(klass=="hkbProjectData"){
  auto i=old.find("attachmentSetups");if(i!=old.end()){Need(count(i->second)==0,"Original project attachmentSetups require a dedicated native attachment port");consumed.insert(i->first);++migrations;}
 }
 if(klass=="hkbStateMachineStateInfo")for(const auto&[before,after]:{std::pair{"enterNotifyEvent","enterNotifyEvents"},std::pair{"exitNotifyEvent","exitNotifyEvents"}}){
  auto i=old.find(before),j=now.find(after);if(i==old.end())continue;Need(j!=now.end()&&i->second.type==25&&j->second.type==20&&j->second.klass=="hkbStateMachineEventPropertyArray","Native state event-array schema differs");
  auto eventFields=Fields(i->second.klass,false);Need(eventFields.contains("id")&&eventFields.contains("payload"),"Original state notification event schema differs");
  auto from=Pack::Add(at,i->second.offset);auto id=source.Word(from,eventFields.at("id").offset);auto payload=source.Pointer(Pack::Add(from,eventFields.at("payload").offset));
  if(id!=UINT32_MAX||payload!=At{}){const auto block=NewObject(j->second.klass);Link(dst+j->second.offset,block);auto targetFields=Fields(j->second.klass,true);Need(targetFields.contains("events"),"Native state event-array has no events member");const auto& array=targetFields.at("events");Need(array.type==22&&array.subtype==25&&array.klass==i->second.klass,"Native state event-array element differs");auto value=Allocate(Schema(array.klass,true).size);Value(from,i->second.klass,value);Link(block+array.offset,value);W(data,block+array.offset+4,1);W(data,block+array.offset+8,0x80000001);}
  consumed.insert(before);generated.insert(after);++migrations;
 }
 if(klass=="hkbBehaviorGraphData"&&old.contains("wordMinVariableValues")){
  const auto&lo=old.at("wordMinVariableValues");const auto&hi=old.at("wordMaxVariableValues");Need(now.contains("variableBounds"),"Native variable bounds field absent");const auto&bound=now.at("variableBounds");auto n=count(lo);Need(n==count(hi)&&n<=1000000,"Original variable bounds arrays disagree");Need(bound.type==22&&bound.subtype==25&&bound.klass=="hkbVariableBounds"&&lo.type==22&&lo.subtype==25&&hi.type==22&&hi.subtype==25,"Variable bounds semantic schema differs");
  W(data,dst+bound.offset+4,n);W(data,dst+bound.offset+8,n|0x80000000);if(n){auto low=source.Pointer(Pack::Add(at,lo.offset)),high=source.Pointer(Pack::Add(at,hi.offset));Need(low!=At{}&&high!=At{},"Original variable bound storage absent");auto fields=Fields(bound.klass,true);Need(fields.contains("min")&&fields.contains("max"),"Native bound pair schema differs");const auto stride=Schema(bound.klass,true).size,os=Schema(lo.klass,false).size;Need(hi.klass==lo.klass,"Original bounds have different value types");Need(uint64_t(n)*stride<=64*1024*1024,"Native variable bound byte budget exceeded");auto values=Allocate(n*stride);Link(dst+bound.offset,values);for(uint32_t i=0;i<n;++i){Value(Pack::Add(low,i*os),lo.klass,values+i*stride+fields.at("min").offset);Value(Pack::Add(high,i*os),hi.klass,values+i*stride+fields.at("max").offset);}}
  consumed.insert("wordMinVariableValues");consumed.insert("wordMaxVariableValues");generated.insert("variableBounds");++migrations;
 }
 if(klass=="hkbCharacterStringData"&&old.contains("deformableSkinNames")){
  auto a=old.at("deformableSkinNames"),b=old.at("rigidSkinNames");Need(now.contains("skinNames"),"Native character skin names field absent");Need(!(count(a)&&count(b)),"Simultaneous deformable/rigid skin-name ordering requires a verified semantic adapter");auto chosen=count(a)?a:b;auto target=now.at("skinNames");Member(Pack::Add(at,chosen.offset),chosen,target,dst+target.offset,klass);consumed.insert(a.name);consumed.insert(b.name);generated.insert(target.name);++migrations;
 }
 const std::set<std::string> emptyDefaults={"hkbProjectStringData.scriptsPath","hkbProjectStringData.fullPathToSource","hkbStateMachine.startStateChooser","hkbCharacterStringData.boneAttachmentNames","hkbCharacterStringData.mirroredSyncPointSubstringsA","hkbCharacterStringData.mirroredSyncPointSubstringsB","hkbCharacterStringData.luaScriptOnCharacterActivated","hkbCharacterStringData.luaScriptOnCharacterDeactivated","hkbCharacterStringData.luaFiles","hkbCharacterData.characterControllerInfo","hkbCharacterData.footIkDriverInfo","hkbCharacterData.handIkDriverInfo","hkbCharacterData.boneAttachmentBoneIndices","hkbCharacterData.boneAttachmentTransforms"};
 for(const auto&[name,m]:now){Need(uint64_t(m.offset)+Size(m,true)<=newSchema.size,"Native member exceeds class extent: "+klass+"."+name);if(m.flags&0x400){
   auto i=old.find(name);
   if(i!=old.end()&&!(i->second.flags&0x400)){
    Need((klass=="hkbExpressionCondition"||klass=="hkbEvaluateExpressionModifier")&&name=="compiledExpressionSet"&&i->second.type==20&&i->second.klass=="hkbCompiledExpressionSet","Serialized source field became native-runtime-only without a semantic adapter: "+klass+"."+name);
    auto cache=source.Pointer(Pack::Add(at,i->second.offset));if(cache!=At{}){Need(source.Class(cache)=="hkbCompiledExpressionSet","Expression cache has unexpected source class");retired_runtime_objects.insert(cache);}consumed.insert(name);++migrations;
   }
   continue;
  }if(generated.contains(name))continue;auto i=old.find(name);
  if(i!=old.end()&&!(i->second.flags&0x400)){Member(Pack::Add(at,i->second.offset),i->second,m,dst+m.offset,klass);consumed.insert(name);continue;}
  if(klass=="hkbCharacterData"&&name=="characterControllerInfo"){
   Need(m.type==25&&m.klass=="hkbCharacterDataCharacterControllerInfo","Native new character-controller schema differs");Defaults(m.klass,dst+m.offset);++migrations;continue;
  }
  if(m.default_bytes){Need(m.default_bytes->empty()||m.default_bytes->size()==Size(m,true),"Native reflected default has incorrect extent");if(!m.default_bytes->empty())std::copy(m.default_bytes->begin(),m.default_bytes->end(),data.begin()+dst+m.offset);continue;}
  if(klass=="hkbProjectData"&&name=="defaultEventMode"){Need(m.type==24&&std::any_of(m.enumeration.begin(),m.enumeration.end(),[](const auto&e){return e.value==0&&e.name=="EVENT_MODE_DEFAULT";}),"Native default event mode semantics unavailable");++migrations;continue;}
  Need(emptyDefaults.contains(klass+"."+name),"Native behavior field requires verified initialization: "+klass+"."+name);Need(m.type==20||m.type==22||m.type==33,"Explicit empty behavior initialization has nonempty semantics");++migrations;
 }
 for(const auto&[name,m]:old)if(!(m.flags&0x400)&&!consumed.contains(name))Need(false,"Serialized original behavior field has no native semantic mapping: "+klass+"."+name);
}
} // namespace
bool UpgradeBehaviorResource(std::span<const uint8_t> original,const NativeBehaviorSchemas& schemas,BehaviorUpgradeResult& result,std::string& error){
 try{Converter c(original,schemas);auto root=c.Object({2,U(original,28)},"hkRootLevelContainer");
  for(const auto&[at,name]:c.source.objects)if(at.section==2)Need(c.relocated.contains(at)||c.retired_runtime_objects.contains(at),"Unreachable original behavior object cannot be silently discarded: "+name);
  BehaviorUpgradeResult ready;ready.pack=c.Finish(root);ready.dependencies=std::move(c.dependencies);ready.objects=uint32_t(c.relocated.size());ready.semantic_migrations=c.migrations;result=std::move(ready);error.clear();return true;
 }catch(const std::exception&e){error=e.what();return false;}
}
bool TestBehaviorUpgrade(std::span<const uint8_t> original,const NativeBehaviorSchemas& schemas,std::string& error){
 try{BehaviorUpgradeResult a,b;std::string why;Need(UpgradeBehaviorResource(original,schemas,a,why),why);Need(UpgradeBehaviorResource(original,schemas,b,why),why);Need(a.pack==b.pack&&a.objects>0,"Behavior conversion is nondeterministic/empty");Need(U(a.pack,12)==9&&U(a.pack,16)==0x04000001&&std::memcmp(a.pack.data()+40,"hk_2011.2.0-r1",13)==0,"Native behavior pack ABI differs");Pack after(a.pack,false);Need(after.Class({2,U(a.pack,28)})=="hkRootLevelContainer","Converted behavior root/fixups invalid");
  for(const auto&[at,klass]:after.objects){Need(at.section==2&&schemas.classes.contains(klass),"Converted finish class lacks target schema");const auto&target=schemas.classes.at(klass);Need(target.signature_verified&&at.offset+uint64_t(target.size)<=after.sections[2].local,"Converted object extent/signature invalid");}
  for(const auto&d:a.dependencies)Need(after.Text({2,d.native_string_offset})==d.path,"Dependency string/remap location differs from native pack");
  auto changed=schemas;BehaviorMember unsupported;unsupported.name="unverifiedNewField";unsupported.type=7;unsupported.offset=uint16_t(changed.classes.at("hkRootLevelContainer").size);changed.classes.at("hkRootLevelContainer").size+=4;changed.classes.at("hkRootLevelContainer").members.push_back(unsupported);BehaviorUpgradeResult untouched;untouched.pack={1,2,3};Need(!UpgradeBehaviorResource(original,changed,untouched,why)&&untouched.pack==Bytes({1,2,3}),"Unknown serialized target member was guessed or changed output on failure");

  Converter semantic(original,schemas);semantic.Object({2,U(original,28)},"hkRootLevelContainer");bool testedSetOnce=false;
  for(const auto&[at,klass]:semantic.source.objects)if(at.section==2){
   if(klass=="hkbStateMachine"){
    const auto sourceFields=semantic.Fields(klass,false),targetFields=semantic.Fields(klass,true);const auto oldMode=semantic.source.Data(Pack::Add(at,sourceFields.at("startStateMode").offset),1)[0];const auto target=semantic.relocated.at(at).second+targetFields.at("startStateMode").offset;
    Need(semantic.data[target]==(oldMode==0?0:oldMode==2?1:oldMode==3?2:255),"State-machine start mode was ordinal-copied instead of semantically mapped");
    if(!testedSetOnce){auto unsupportedOnce=Bytes(original.begin(),original.end());unsupportedOnce[semantic.source.sections[2].start+at.offset+sourceFields.at("startStateMode").offset]=1;BehaviorUpgradeResult unchanged;unchanged.pack={7};Need(!UpgradeBehaviorResource(unsupportedOnce,schemas,unchanged,why)&&unchanged.pack==Bytes({7}),"Unsupported old SET_ONCE mode was guessed/changed output");testedSetOnce=true;}
   }
   if(klass=="hkbCharacterData"){
    const auto targetFields=semantic.Fields(klass,true);const auto&controller=targetFields.at("characterControllerInfo");const auto target=semantic.relocated.at(at).second+controller.offset;
    for(const auto&[name,member]:semantic.Fields(controller.klass,true))if(member.default_bytes&&!member.default_bytes->empty())Need(std::equal(member.default_bytes->begin(),member.default_bytes->end(),semantic.data.begin()+target+member.offset),"New native character-controller reflected default was replaced with zeros");
    auto missingDefault=schemas;for(auto&member:missingDefault.classes.at(controller.klass).members)if(member.name=="capsuleRadius")member.default_bytes.reset();BehaviorUpgradeResult rejectedDefault;Need(!UpgradeBehaviorResource(original,missingDefault,rejectedDefault,why)&&rejectedDefault.pack.empty(),"Missing new native capsule default was guessed");
   }
  }
  auto foreign=Bytes(original.begin(),original.end());for(const auto&[at,klass]:semantic.source.objects)if(at.section==2&&klass.starts_with("hkb")){auto begin=foreign.begin()+semantic.source.sections[0].start,end=begin+semantic.source.sections[0].local;auto match=std::search(begin,end,klass.begin(),klass.end());Need(match!=end,"Source class name fixture missing");match[2]='a';BehaviorUpgradeResult rejectedForeign;Need(!UpgradeBehaviorResource(foreign,schemas,rejectedForeign,why)&&rejectedForeign.pack.empty(),"Non-HKB resource admitted through behavior-only converter");break;}
  auto badVersion=Bytes(original.begin(),original.end());badVersion[53]='2';BehaviorUpgradeResult badVersionOut;Need(!UpgradeBehaviorResource(badVersion,schemas,badVersionOut,why)&&badVersionOut.pack.empty(),"Wrong source Havok release accepted");
  auto corrupted=Bytes(original.begin(),original.end());corrupted[18]=0;BehaviorUpgradeResult rejected;Need(!UpgradeBehaviorResource(corrupted,schemas,rejected,why)&&!why.empty()&&rejected.pack.empty(),"Wrong original behavior layout was accepted");
  auto absent=schemas;absent.classes.erase("hkRootLevelContainer");Need(!UpgradeBehaviorResource(original,absent,rejected,why)&&rejected.pack.empty(),"Unverified target schema accepted");
  auto unverified=schemas;unverified.classes.at("hkRootLevelContainer").signature_verified=false;Need(!UpgradeBehaviorResource(original,unverified,rejected,why)&&rejected.pack.empty(),"Unverified target class signature accepted");
  error.clear();return true;
 }catch(const std::exception&e){error=e.what();return false;}
}
} // namespace em2::em1
