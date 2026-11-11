#include "em1_behavior_probe.h"
#include "em1_behavior_schemas.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>
namespace em2::em1 {namespace {
void Require(bool okay,const std::string&why){if(!okay)throw std::runtime_error(why);}
uint32_t Read(std::span<const uint8_t>b,size_t at){Require(at<=b.size()&&4<=b.size()-at,"Behavior probe pack read outside payload");return(uint32_t(b[at])<<24)|(uint32_t(b[at+1])<<16)|(uint32_t(b[at+2])<<8)|b[at+3];}
bool Mapped(uint8_t*base,uint32_t at,size_t n){return base&&at>=4096&&n&&uint64_t(at)+n<=0x100000000ull&&GuestReadBatch::Readable(base+at,n);}
struct Preserve{PPCContext&ctx;PPCContext saved;DWORD error=GetLastError();unsigned csr=_mm_getcsr();Preserve(PPCContext&c):ctx(c),saved(c){}~Preserve(){ctx=saved;SetLastError(error);_mm_setcsr(csr);}};
struct View{
 std::span<const uint8_t>b,data;std::map<uint32_t,uint32_t>pointers;std::map<uint32_t,std::string>objects;
 View(std::span<const uint8_t>bytes):b(bytes){Require(b.size()>=208&&Read(b,20)==3,"Behavior probe expected a three-section pack");auto start=Read(b,180),local=Read(b,184),global=Read(b,188),virt=Read(b,192),end=Read(b,196);Require(local<=global&&global<=virt&&virt<=end&&uint64_t(start)+end<=b.size(),"Behavior probe section extent invalid");data=b.subspan(start,local);
  for(uint32_t p=local;p+8<=global;p+=8){auto field=Read(b,start+p);if(field==UINT32_MAX)break;Require(field+uint64_t(4)<=local&&Read(b,start+p+4)<local,"Behavior probe local fixup invalid");Require(pointers.emplace(field,Read(b,start+p+4)).second,"Duplicate behavior probe pointer");}
  for(uint32_t p=global;p+12<=virt;p+=12){auto field=Read(b,start+p);if(field==UINT32_MAX)break;Require(Read(b,start+p+4)==2&&field+uint64_t(4)<=local&&Read(b,start+p+8)<local,"Behavior probe cross-section pointer unsupported");Require(pointers.emplace(field,Read(b,start+p+8)).second,"Duplicate behavior probe pointer");}
  for(uint32_t p=virt;p+12<=end;p+=12){auto field=Read(b,start+p);if(field==UINT32_MAX)break;auto sec=Read(b,start+p+4),off=Read(b,start+p+8);Require(sec<3&&field<local,"Behavior probe finish class invalid");auto base=Read(b,84+sec*48),size=Read(b,88+sec*48);Require(off<size&&uint64_t(base)+size<=b.size(),"Behavior probe class name extent invalid");auto begin=b.begin()+base+off,stop=std::find(begin,b.begin()+base+size,0);Require(stop!=b.begin()+base+size,"Behavior probe class name unterminated");Require(objects.emplace(field,std::string(begin,stop)).second,"Duplicate behavior probe finish class");}
 }
};
struct Verify{
 uint8_t*base;uint32_t origin;const View&view;const NativeBehaviorSchemas&schemas;uint32_t fields=0;
 const NativeBehaviorClass&Class(const std::string&n){auto i=schemas.classes.find(n);Require(i!=schemas.classes.end(),"Behavior loaded class lacks checked reflection: "+n);return i->second;}
 uint32_t Size(const BehaviorMember&m){auto t=m.type==24||m.type==31?m.subtype:m.type;uint32_t n=0;switch(t){case 1:case 2:case 3:case 4:n=1;break;case 5:case 6:case 32:n=2;break;case 7:case 8:case 11:case 20:case 21:case 29:case 30:case 33:n=4;break;case 9:case 10:n=8;break;case 12:case 13:n=16;break;case 14:case 15:case 16:n=48;break;case 17:case 18:n=64;break;case 22:n=12;break;case 26:case 28:n=8;break;case 25:n=Class(m.klass).size;break;default:throw std::runtime_error("Unsupported native behavior probe member type");}Require(!m.array_size||n<=65536/m.array_size,"Behavior probe inline extent overflow");return n*(m.array_size?m.array_size:1);}
 void Bytes(uint32_t at,uint32_t n){Require(uint64_t(at)+n<=view.data.size()&&uint64_t(origin)+at+n<=0x100000000ull&&Mapped(base,origin+at,n),"Native behavior field is not resident");Require(!std::memcmp(base+origin+at,view.data.data()+at,n),"Native finish changed authored behavior bytes at "+std::to_string(at));++fields;}
 void Pointer(uint32_t at){auto i=view.pointers.find(at);Require(Mapped(base,origin+at,4),"Native behavior pointer is unreadable");auto expected=i==view.pointers.end()?0:origin+i->second;Require(PPC_LOAD_U32(origin+at)==expected,"Native behavior pointer fixup differs at "+std::to_string(at));++fields;}
 void Member(uint32_t at,BehaviorMember m,unsigned depth){Require(depth<64,"Behavior probe struct recursion exceeds budget");if(m.array_size){auto n=m.array_size;m.array_size=0;auto stride=Size(m);for(uint32_t i=0;i<n;++i)Member(at+i*stride,m,depth+1);return;}
  if(m.type==25){Value(at,m.klass,depth+1);return;}if(m.type==20){Pointer(at);return;}
  if(m.type==29||m.type==33){Pointer(at);auto p=view.pointers.find(at);if(p!=view.pointers.end()){auto begin=view.data.begin()+p->second,stop=std::find(begin,view.data.end(),0);Require(stop!=view.data.end()&&stop-begin<=4096,"Behavior probe string extent invalid");Bytes(p->second,uint32_t(stop-begin+1));}return;}
  if(m.type==22||m.type==26){Pointer(at);Bytes(at+4,m.type==22?8:4);auto n=Read(view.data,at+4);Require(n<=1000000,"Native behavior probe array budget exceeded");if(!n)return;auto p=view.pointers.find(at);Require(p!=view.pointers.end(),"Behavior array has no converted storage");m.type=m.subtype;auto stride=Size(m);Require(uint64_t(p->second)+uint64_t(n)*stride<=view.data.size(),"Behavior array exceeds converted data");for(uint32_t i=0;i<n;++i)Member(p->second+i*stride,m,depth+1);return;}
  Bytes(at,Size(m));
 }
 void Value(uint32_t at,const std::string&klass,unsigned depth=0){Require(depth<64,"Behavior class inheritance exceeds budget");const auto&c=Class(klass);Require(uint64_t(at)+c.size<=view.data.size(),"Behavior class outside converted data");if(!c.parent.empty())Value(at,c.parent,depth+1);for(const auto&m:c.members)if(!(m.flags&0x400))Member(at+m.offset,m,depth+1);}
};
}
bool ProbeNativeBehaviorResources(PPCContext&ctx,uint8_t*base,uint32_t manager,std::shared_ptr<RvzSource>source,std::span<const BehaviorProbeInput>inputs,BehaviorProbeResult&out,std::string&error){
 Preserve preserve(ctx);if(!out.retained.empty()){error="Previous behavior probe still owns native references";return false;}BehaviorProbeResult ready;uint32_t records=0;bool records_known=false;
 try{
  Require(out.retained.empty(),"Previous behavior probe still owns native references");Require(source&&inputs.size()>0&&inputs.size()<=8&&Mapped(base,manager,1468),"Behavior probe needs owned source and native manager");records=PPC_LOAD_U32(manager+1464);records_known=true;
  std::set<std::string>names;for(const auto&input:inputs){Require(!input.origin.empty(),"Behavior probe input has no source origin");View v(input.original);for(const auto&[at,name]:v.objects)names.insert(name);}std::vector<std::string>requested(names.begin(),names.end());NativeBehaviorSchemas schemas;Require(ReadNativeBehaviorSchemas(ctx,base,requested,schemas,error),error);Require(TestNativeBehaviorSchemaRejection(error),error);
  for(const auto&input:inputs){BehaviorProbeResourceResult row;row.origin=input.origin;BehaviorUpgradeResult converted;
   try{Require(TestBehaviorUpgrade(input.original,schemas,row.error),row.error);row.cpu_checked=true;Require(UpgradeBehaviorResource(input.original,schemas,converted,row.error),row.error);row.converted=true;row.objects=converted.objects;row.semantic_migrations=converted.semantic_migrations;row.dependencies=uint32_t(converted.dependencies.size());ready.retained.emplace_back();auto&resource=ready.retained.back();row.loaded=PublishNativeHavok(ctx,base,manager,source,input.origin+":behavior-upgrade1",converted.pack,resource,row.error);Require(row.loaded,row.error);View v(converted.pack);const auto root=Read(converted.pack,28);Require(resource.owner.root>=root,"Native behavior root offset overflow");Require(uint64_t(resource.owner.root-root)+v.data.size()<=0x100000000ull,"Native behavior data address overflows");Verify verify{base,resource.owner.root-root,v,schemas};for(const auto&[at,klass]:v.objects)verify.Value(at,klass);row.verified_fields=verify.fields;row.fields_match=true;
   }catch(const std::exception&e){row.error=e.what();}
   if(!ready.retained.empty()){std::string cleanup;if(ReleaseNativeHavok(ctx,base,ready.retained.back(),cleanup)){ready.retained.pop_back();row.released=true;}else{if(!row.error.empty())row.error+="; ";row.error+="native behavior cleanup: "+cleanup;}}
   ready.resources.push_back(std::move(row));if(!ready.retained.empty())break;
  }
  ready.records_restored=Mapped(base,manager,1468)&&PPC_LOAD_U32(manager+1464)==records;bool okay=ready.resources.size()==inputs.size()&&ready.retained.empty()&&ready.records_restored;for(const auto&r:ready.resources)okay=okay&&r.cpu_checked&&r.converted&&r.loaded&&r.fields_match&&r.released;if(!okay)error="Native behavior resource conversion/load/readback/release proof incomplete";else error.clear();out=std::move(ready);return okay;
 }catch(const std::exception&e){error=e.what();ready.records_restored=records_known&&Mapped(base,manager,1468)&&PPC_LOAD_U32(manager+1464)==records;out=std::move(ready);return false;}
}
}
