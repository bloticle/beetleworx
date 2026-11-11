#include "em1_native_resources.h"
#include "em1_nif_container.h"
#include "online_runtime.h"
#include "ppc_context.h"
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>

PPC_EXTERN_FUNC(__imp__sub_826549B8); // game allocation(size,alignment,flags)
PPC_EXTERN_FUNC(__imp__sub_82655930); // matching unsized game free
PPC_EXTERN_FUNC(__imp__sub_827499B0); // canonical resource lookup
PPC_EXTERN_FUNC(__imp__sub_83089FE0);
PPC_EXTERN_FUNC(__imp__sub_8274C588); // synchronous typed resource publication
PPC_EXTERN_FUNC(__imp__sub_8274B288); // release native resource reference
PPC_EXTERN_FUNC(__imp__sub_83219080); // HKX named cache lookup, retained wrapper out
PPC_EXTERN_FUNC(__imp__sub_8238C448); // Havok removeReference
PPC_EXTERN_FUNC(__imp__sub_82555E28); // NiStream object-cache accessor
PPC_EXTERN_FUNC(__imp__sub_82539F10); // typed resource handle -> retained NiFixedString name
PPC_EXTERN_FUNC(__imp__sub_822F1C10); // NiFixedString destructor
PPC_EXTERN_FUNC(__imp__sub_83009FB8); // original KF string-keyed cache lookup(map,key,out)
PPC_EXTERN_FUNC(__imp__sub_82808930); // original NiActorManager sequence-ID lookup
PPC_EXTERN_FUNC(__imp__sub_8252FFF8); // original NiActorManager::SetTargetSequence(id)
PPC_EXTERN_FUNC(__imp__sub_8300DF18); // original NiActorManager::Update(absoluteTime)
PPC_EXTERN_FUNC(__imp__sub_82552F00); // NiAVObject::Update(time,updateControllers)

namespace em2::em1 {
namespace {
constexpr uint32_t kStack=32768;
std::atomic_uint64_t next_name{1};
thread_local bool publishing=false;
struct PublicationScope { bool previous=publishing; PublicationScope(){publishing=true;} ~PublicationScope(){publishing=previous;} };
void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
bool Mapped(uint8_t* base,uint32_t address,size_t bytes,bool writable=false) {
  if(!base || address<4096 || !bytes || uint64_t(address)+bytes>0x100000000ull)return false;
  auto at=reinterpret_cast<uintptr_t>(base+address);const auto end=at+bytes;
  while(at<end) {
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
        (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto access=info.Protect&255;
    const bool write=access==PAGE_READWRITE || access==PAGE_WRITECOPY ||
        access==PAGE_EXECUTE_READWRITE || access==PAGE_EXECUTE_WRITECOPY;
    if(writable ? !write : !(write || access==PAGE_READONLY || access==PAGE_EXECUTE_READ))return false;
    const auto next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
    if(next<=at)return false;at=std::min(end,next);
  }
  return true;
}
struct Restore {
  PPCContext& ctx;PPCContext saved;unsigned csr;DWORD error;
  explicit Restore(PPCContext& c):ctx(c),saved(c),csr(_mm_getcsr()),error(GetLastError()){}
  ~Restore(){ctx=saved;_mm_setcsr(csr);SetLastError(error);}
};
struct Names {
  rex::memory::Memory& memory;uint32_t address;
  explicit Names(rex::memory::Memory& m):memory(m),address(m.SystemHeapAlloc(512,32)){}
  ~Names(){if(address)memory.SystemHeapFree(address);}
};
void Prepare(PPCContext& ctx,uint8_t* base,uint32_t manager) {
  Require(!em2::net::SynchronizedSessionActive(),"EM1 preview resources are unavailable in online sessions");
  Require(ctx.r1.u32>=kStack && Mapped(base,ctx.r1.u32-kStack,kStack,true),"Native resource call lacks writable guest stack");
  Require(Mapped(base,manager,1804,true) && Mapped(base,PPC_LOAD_U32(manager),80),"Invalid native pack manager");
  ctx.r1.u64-=2048;
}
uint32_t Lookup(PPCContext& ctx,uint8_t* base,uint32_t manager,uint32_t name) {
  PPC_STORE_U32(name+256,0);
  ctx.r3.u64=manager;ctx.r4.u64=name;ctx.r5.u64=name+256;ctx.r6.u64=0;
  __imp__sub_827499B0(ctx,base);return ctx.r3.u32;
}
void Drop(PPCContext& ctx,uint8_t* base,uint32_t manager,uint32_t name) {
  ctx.r3.u64=manager;ctx.r4.u64=name;__imp__sub_8274B288(ctx,base);
}
uint32_t TextureLookup(PPCContext& ctx,uint8_t* base,uint32_t manager,uint32_t key,std::string& error){
  ctx.r3.u64=manager;ctx.r4.u64=0x4e494600;__imp__sub_83089FE0(ctx,base);
  const auto loader=ctx.r3.u32;
  if(!Mapped(base,loader,0x21c) || PPC_LOAD_U32(loader)!=0x822259A8){error="Unexpected native NIF texture loader";return 0;}
  ctx.r3.u64=loader+0x44;__imp__sub_82555E28(ctx,base);const auto cache=ctx.r3.u32;
  if(!Mapped(base,cache,4) || !Mapped(base,PPC_LOAD_U32(cache),24)){error="Native NIF object cache unavailable";return 0;}
  const auto lookup=PPC_LOAD_U32(PPC_LOAD_U32(cache)+16);
  if(lookup<0x822f0000 || lookup>=0x83350000){error="Invalid native texture cache lookup";return 0;}
  ctx.r3.u64=cache;ctx.r4.u64=key;ctx.r5.u64=0;PPC_CALL_INDIRECT_FUNC(lookup);
  return ctx.r3.u32;
}
}

bool NativeModelPublicationActive(){return publishing;}
std::string ReserveNativeModelName(){return "em1/runtime/"+std::to_string(next_name.fetch_add(1))+".nif";}
std::string ReserveNativeKeyframeName(){return "em1/runtime/"+std::to_string(next_name.fetch_add(1))+".kf";}
std::string ReserveNativeAnimationManagerName(){return "em1/runtime/"+std::to_string(next_name.fetch_add(1))+".kfm";}

bool SelectNativeAnimationSequence(PPCContext& ctx,uint8_t* base,const NativeModelResource& resource,uint32_t sequence,std::string& error){
  Restore restore(ctx);error.clear();
  try{Require(Mapped(base,resource.record,28) && PPC_LOAD_U32(resource.record+12)>0 && PPC_LOAD_U32(resource.record+20)==resource.handle &&
      Mapped(base,resource.handle,20) && PPC_LOAD_U32(resource.handle)==0x8203d0b4 && PPC_LOAD_U32(resource.handle+16)==resource.root &&
      Mapped(base,resource.root,0xac) && PPC_LOAD_U32(resource.root)==0x821d4bf8,"Invalid native actor-animation manager");Prepare(ctx,base,resource.manager);}
  catch(const std::exception& e){error=e.what();return false;}
  ctx.r3.u64=resource.root;ctx.r4.u64=sequence;__imp__sub_8252FFF8(ctx,base);
  if(!(ctx.r3.u32&255)){error="Original actor manager rejected sequence ID";return false;}return true;
}
bool UpdateNativeAnimation(PPCContext& ctx,uint8_t* base,const NativeModelResource& resource,float time,std::string& error){
  Restore restore(ctx);error.clear();
  try{Require(std::isfinite(time) && time>=0 && Mapped(base,resource.record,28) && PPC_LOAD_U32(resource.record+12)>0 && PPC_LOAD_U32(resource.record+20)==resource.handle &&
      Mapped(base,resource.handle,20) && PPC_LOAD_U32(resource.handle)==0x8203d0b4 && PPC_LOAD_U32(resource.handle+16)==resource.root &&
      Mapped(base,resource.root,0xac) && PPC_LOAD_U32(resource.root)==0x821d4bf8,"Invalid native animation time/owner");Prepare(ctx,base,resource.manager);}
  catch(const std::exception& e){error=e.what();return false;}
  const auto root=PPC_LOAD_U32(resource.root+0x14);if(!Mapped(base,root,208)){error="Native animation model root unavailable";return false;}
  ctx.r3.u64=resource.root;ctx.f1.f64=time;__imp__sub_8300DF18(ctx,base);
  ctx.r3.u64=root;ctx.f1.f64=time;ctx.r5.u64=1;__imp__sub_82552F00(ctx,base);return true;
}

bool PublishNativeAnimationManager(PPCContext& ctx,uint8_t* base,uint32_t manager,std::shared_ptr<RvzSource> source,
    const BoundKfmResource& prepared,std::string_view native_name,NativeModelResource& output,std::string& error){
  Restore restore(ctx);error.clear();auto* kernel=rex::kernel::kernel_state();KfmDocument document;
  const auto name=std::string(native_name);
  try{
    Require(!output.record && source && !prepared.origin_key.empty(),"Invalid animation manager owner");
    Require(kernel && kernel->memory(),"Native memory unavailable");
    Require(name.starts_with("em1/runtime/") && name.ends_with(".kfm") && name.size()<240 && name.find("..") ==name.npos &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789/.-")==name.npos,"Invalid native animation manager name");
    Require(InspectKfm(prepared.bytes,document,error),"Invalid rebound KFM payload");Prepare(ctx,base,manager);
  }catch(const std::exception& e){if(error.empty())error=e.what();return false;}
  Names scratch(*kernel->memory());if(!scratch.address || !Mapped(base,scratch.address,512,true)){error="Animation manager scratch unavailable";return false;}
  std::memset(base+scratch.address,0,512);std::memcpy(base+scratch.address,name.c_str(),name.size()+1);
  if(Lookup(ctx,base,manager,scratch.address)){error="Animation manager name already exists";return false;}
  ctx.r3.u64=manager;ctx.r4.u64=0x4b464d00;__imp__sub_83089FE0(ctx,base);const auto loader=ctx.r3.u32;
  if(!Mapped(base,loader,0x64) || PPC_LOAD_U32(loader)!=0x82227bd0 || PPC_LOAD_U32(0x82227bd0+72)!=0x83222500){error="Unexpected KFM factory ABI";return false;}
  ctx.r3.u64=prepared.bytes.size();ctx.r4.u64=32;ctx.r5.u64=0;__imp__sub_826549B8(ctx,base);const auto data=ctx.r3.u32;
  if(!data){error="Game animation manager allocation failed";return false;}
  if(!Mapped(base,data,prepared.bytes.size(),true)){ctx.r3.u64=data;__imp__sub_82655930(ctx,base);error="Animation manager allocation not writable";return false;}
  std::memcpy(base+data,prepared.bytes.data(),prepared.bytes.size());
  ctx.r3.u64=manager;ctx.r4.u64=scratch.address;ctx.r5.u64=0x4b464d00;ctx.r6.u64=data;ctx.r7.u64=prepared.bytes.size();ctx.r8.u64=prepared.bytes.size();
  {PublicationScope publication;__imp__sub_8274C588(ctx,base);}
  const auto record=Lookup(ctx,base,manager,scratch.address);
  if(!Mapped(base,record,28)){error="Native KFM publication produced no record";return false;}
  const auto handle=PPC_LOAD_U32(record+20);
  if(!Mapped(base,handle,20) || PPC_LOAD_U32(handle)!=0x8203d0b4 || !(PPC_LOAD_U32(record+16)&1) || PPC_LOAD_U32(record+12)!=1){Drop(ctx,base,manager,scratch.address);error="Native KFM handle invalid";return false;}
  const auto realize=PPC_LOAD_U32(PPC_LOAD_U32(manager)+36);
  if(realize<0x822f0000 || realize>=0x83350000){Drop(ctx,base,manager,scratch.address);error="Native KFM realization unavailable";return false;}
  ctx.r3.u64=manager;ctx.r4.u64=handle;PPC_CALL_INDIRECT_FUNC(realize);const bool realized=(ctx.r3.u32&255)!=0;
  // KFM handles store a uniqueness byte at+12 and NiActorManager at+16.
  // This is a different native handle class from the NIF graph handle.
  const auto root=PPC_LOAD_U32(handle+16);
  REXLOG_INFO("[em1] KFM native realization: realized={} handle={:08X} table={:08X} actor={:08X}",realized,handle,PPC_LOAD_U32(handle),root);
  if(!realized || !Mapped(base,root,0xac) || PPC_LOAD_U32(root)!=0x821d4bf8 || !PPC_LOAD_U32(root+4)){Drop(ctx,base,manager,scratch.address);error="Native KFM did not realize an animation manager";return false;}
  bool bound=Mapped(base,PPC_LOAD_U32(root+0x14),16) && Mapped(base,PPC_LOAD_U32(root+0x18),16);
  for(const auto id:document.sequence_ids){if(!bound)break;ctx.r3.u64=root;ctx.r4.u64=id;__imp__sub_82808930(ctx,base);const auto sequence=ctx.r3.u32;
    bound=Mapped(base,sequence,0x48) && PPC_LOAD_U32(sequence)==0x821d4c28;}
  if(!bound){Drop(ctx,base,manager,scratch.address);error="Native KFM did not bind its original model/controllers/sequences";return false;}
  output={std::move(source),prepared.origin_key,name,manager,record,handle,root};error.clear();return true;
}

static uint32_t KeyframeLookup(PPCContext& ctx,uint8_t* base,uint32_t manager,uint32_t key,uint32_t out,std::string& error){
  ctx.r3.u64=manager;ctx.r4.u64=0x4b460000;__imp__sub_83089FE0(ctx,base);const auto loader=ctx.r3.u32;
  if(!Mapped(base,loader,0x40) || PPC_LOAD_U32(loader)!=0x82227d98 || !PPC_LOAD_U32(loader+0x10)){
    error="Original KF loader/cache is unavailable";return 0;
  }
  PPC_STORE_U32(out,0);ctx.r3.u64=loader+0xc;ctx.r4.u64=key;ctx.r5.u64=out;__imp__sub_83009FB8(ctx,base);
  return (ctx.r3.u32&255)?PPC_LOAD_U32(out):0;
}
bool PublishNativeKeyframe(PPCContext& ctx,uint8_t* base,uint32_t manager,std::shared_ptr<RvzSource> source,const KeyframeResource& prepared,
    NativeKeyframeResource& output,std::string& error,std::string_view reserved_name){
  Restore restore(ctx);error.clear();auto* kernel=rex::kernel::kernel_state();KeyframeDocument document;
  const auto name=reserved_name.empty()?ReserveNativeKeyframeName():std::string(reserved_name);
  try{
    Require(!output.owner.record && source && !prepared.origin_key.empty(),"Invalid keyframe owner");
    Require(kernel && kernel->memory(),"Native memory is unavailable");
    Require(name.starts_with("em1/runtime/") && name.ends_with(".kf") && name.size()<240 && name.find("..") ==name.npos &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789/.-")==name.npos,"Invalid native keyframe name");
    Require(InspectKeyframe(prepared.bytes,document,error),"Invalid original keyframe payload");Prepare(ctx,base,manager);
  }catch(const std::exception& e){if(error.empty())error=e.what();return false;}
  Names scratch(*kernel->memory());if(!scratch.address || !Mapped(base,scratch.address,512,true)){error="Keyframe name allocation failed";return false;}
  std::memset(base+scratch.address,0,512);std::memcpy(base+scratch.address,name.c_str(),name.size()+1);
  if(Lookup(ctx,base,manager,scratch.address)){error="Native keyframe name already exists";return false;}
  ctx.r3.u64=manager;ctx.r4.u64=0x4b460000;__imp__sub_83089FE0(ctx,base);const auto loader=ctx.r3.u32;
  if(!Mapped(base,loader,0x40) || PPC_LOAD_U32(loader)!=0x82227d98 || PPC_LOAD_U32(0x82227d98+72)!=0x83222f88){error="Unexpected KF factory ABI";return false;}
  ctx.r3.u64=prepared.bytes.size();ctx.r4.u64=32;ctx.r5.u64=0;__imp__sub_826549B8(ctx,base);const auto data=ctx.r3.u32;
  if(!data){error="Game keyframe allocation failed";return false;}
  if(!Mapped(base,data,prepared.bytes.size(),true)){ctx.r3.u64=data;__imp__sub_82655930(ctx,base);error="Keyframe allocation is not writable";return false;}
  std::memcpy(base+data,prepared.bytes.data(),prepared.bytes.size());
  ctx.r3.u64=manager;ctx.r4.u64=scratch.address;ctx.r5.u64=0x4b460000;ctx.r6.u64=data;ctx.r7.u64=prepared.bytes.size();ctx.r8.u64=prepared.bytes.size();
  {PublicationScope publication;__imp__sub_8274C588(ctx,base);} // Stock path now owns input allocation.
  const auto record=Lookup(ctx,base,manager,scratch.address);
  if(!Mapped(base,record,28)){error="Native KF resource was not published";return false;}
  const auto handle=PPC_LOAD_U32(record+20);
  if(!Mapped(base,handle,16) || !(PPC_LOAD_U32(record+16)&1) || PPC_LOAD_U32(record+12)!=1){Drop(ctx,base,manager,scratch.address);error="Native KF typed handle invalid";return false;}
  ctx.r3.u64=scratch.address+272;ctx.r4.u64=handle;__imp__sub_82539F10(ctx,base);
  const auto array=KeyframeLookup(ctx,base,manager,PPC_LOAD_U32(scratch.address+272),scratch.address+288,error);
  ctx.r3.u64=scratch.address+272;__imp__sub_822F1C10(ctx,base);
  std::vector<uint32_t> sequences;bool valid=error.empty() && Mapped(base,array,16) && PPC_LOAD_U16(array+10)==document.sequences.size();
  const auto slots=valid?PPC_LOAD_U32(array+4):0;valid=valid && Mapped(base,slots,document.sequences.size()*4);
  if(valid)for(size_t i=0;i<document.sequences.size();++i){const auto object=PPC_LOAD_U32(slots+uint32_t(i)*4);
    if(!Mapped(base,object,0x48) || PPC_LOAD_U32(object)!=0x821d4c28 || !PPC_LOAD_U32(object+4)){valid=false;break;}sequences.push_back(object);}
  if(!valid){Drop(ctx,base,manager,scratch.address);if(error.empty())error="Native KF cache did not retain every original sequence";return false;}
  output.owner={std::move(source),prepared.origin_key,name,manager,record,handle,0};output.array=array;output.sequences=std::move(sequences);error.clear();return true;
}
bool RetainNativeKeyframe(PPCContext& ctx,uint8_t* base,const NativeKeyframeResource& resource,NativeKeyframeResource& output,std::string& error){
  Restore restore(ctx);error.clear();auto* kernel=rex::kernel::kernel_state();
  try{
    Require(!output.owner.record && resource.owner.record && kernel && kernel->memory(),"Invalid keyframe retain owner");
    Require(resource.owner.native_name.starts_with("em1/runtime/") && resource.owner.native_name.size()<240,"Invalid retained keyframe name");
    Prepare(ctx,base,resource.owner.manager);
  }catch(const std::exception& e){error=e.what();return false;}
  Names scratch(*kernel->memory());if(!scratch.address || !Mapped(base,scratch.address,512,true)){error="Keyframe retain scratch unavailable";return false;}
  std::memset(base+scratch.address,0,512);std::memcpy(base+scratch.address,resource.owner.native_name.c_str(),resource.owner.native_name.size()+1);
  const auto record=Lookup(ctx,base,resource.owner.manager,scratch.address);
  if(record!=resource.owner.record || !Mapped(base,record,28) || PPC_LOAD_U32(record+20)!=resource.owner.handle){error="Keyframe owner changed before retention";return false;}
  const auto refs=PPC_LOAD_U32(record+12);if(!refs || refs==UINT32_MAX){error="Invalid retained keyframe reference count";return false;}
  auto retained=resource; // Finish host allocations before acquiring a native reference.
  ctx.r3.u64=resource.owner.manager;ctx.r4.u64=scratch.address;ctx.r5.u64=0x4b460000;ctx.r6.u64=0;ctx.r7.u64=0;ctx.r8.u64=0;__imp__sub_8274C588(ctx,base);
  if(PPC_LOAD_U32(record+12)!=refs+1){error="Original KF reference retention failed";return false;}
  output=std::move(retained);error.clear();return true;
}
bool ReleaseNativeKeyframe(PPCContext& ctx,uint8_t* base,NativeKeyframeResource& resource,std::string& error){
  if(!resource.owner.record){error.clear();return true;}
  Restore restore(ctx);auto* kernel=rex::kernel::kernel_state();
  try{Require(kernel && kernel->memory(),"Native memory is unavailable");Prepare(ctx,base,resource.owner.manager);Require(Mapped(base,resource.owner.handle,16),"Keyframe release handle unavailable");}
  catch(const std::exception& e){error=e.what();return false;}
  if(!Mapped(base,resource.owner.record,28) || !PPC_LOAD_U32(resource.owner.record+12)){error="Keyframe reference count unavailable";return false;}
  const auto references=PPC_LOAD_U32(resource.owner.record+12),original_array=resource.array;
  Names scratch(*kernel->memory());if(!scratch.address || !Mapped(base,scratch.address,512,true)){error="Keyframe release scratch unavailable";return false;}
  std::memset(base+scratch.address,0,512);const auto manager=resource.owner.manager;
  ctx.r3.u64=scratch.address+272;ctx.r4.u64=resource.owner.handle;__imp__sub_82539F10(ctx,base);const auto key=PPC_LOAD_U32(scratch.address+272);
  const bool released=ReleaseNativeModel(ctx,base,resource.owner,error);const auto remaining=released?KeyframeLookup(ctx,base,manager,key,scratch.address+288,error):0;
  ctx.r3.u64=scratch.address+272;__imp__sub_822F1C10(ctx,base);
  if(!released)return false;
  resource={};if((references==1 && remaining) || (references>1 && remaining!=original_array)){error="KF cache lifetime differs from remaining native owners";return false;}return error.empty();
}

static bool PublishNativeNif(PPCContext& ctx,uint8_t* base,uint32_t manager,
                        const ModelClosure& closure,NativeModelResource& output,std::string& error,std::string_view reserved_name,bool texture) {
  Restore restore(ctx);
  error.clear();
  // Catch only host-side preparation/validation failures. Native exceptional
  // exits must propagate: ownership after a partially executed loader is unknown.
  auto* kernel=rex::kernel::kernel_state();
  try {
    Require(!output.record,"Output already owns a native resource reference");
    Require(kernel && kernel->memory(),"Native memory is unavailable");
    Require(closure.source && !closure.model.origin_key.empty() && !closure.model.bytes.empty() &&
        closure.model.bytes.size()<=64*1024*1024,"Invalid or oversized adapted model closure");
    if(texture){const nif::Nif parsed(closure.model.bytes);
      Require(nif::U32(parsed.footer,0,parsed.big)==1,"Texture requires one original root");
      const auto root=nif::U32(parsed.footer,4,parsed.big);
      Require(root<parsed.blocks.size() && parsed.types[parsed.blocks[root].type]=="NiSourceTexture","NIF is not a source-texture resource");}
    Prepare(ctx,base,manager);
  } catch(const std::exception& e){error=e.what();return false;}
  Names scratch(*kernel->memory());
  if(!scratch.address || !Mapped(base,scratch.address,512,true)){error="Native resource name allocation failed";return false;}
  // Names never alias an original EM2 path or a previous source. Keep the full
  // original RVZ/PAK/resource identity separately; this is a runtime handle,
  // not a persistent asset-cache key or a rewritten original resource name.
  const auto name=reserved_name.empty()?ReserveNativeModelName():std::string(reserved_name);
  if(!name.starts_with("em1/runtime/") || !name.ends_with(".nif") || name.size()>=240 ||
      name.find("..")!=std::string::npos || name.find(char(0))!=std::string::npos){error="Invalid reserved model name";return false;}
  std::memset(base+scratch.address,0,512);std::memcpy(base+scratch.address,name.c_str(),name.size()+1);
  if(Lookup(ctx,base,manager,scratch.address)){error="Native EM1 resource name already exists";return false;}
  ctx.r3.u64=closure.model.bytes.size();ctx.r4.u64=32;ctx.r5.u64=0;
  __imp__sub_826549B8(ctx,base);const auto data=ctx.r3.u32;
  if(!data){error="Game allocator could not allocate model bytes";return false;}
  if(!Mapped(base,data,closure.model.bytes.size(),true)) {
    ctx.r3.u64=data;__imp__sub_82655930(ctx,base);error="Game model allocation is not writable";return false;
  }
  std::memcpy(base+data,closure.model.bytes.data(),closure.model.bytes.size());
  ctx.r3.u64=manager;ctx.r4.u64=0x4e494600;__imp__sub_83089FE0(ctx,base);
  const auto loader=ctx.r3.u32;
  const auto vtable=Mapped(base,loader,4)?PPC_LOAD_U32(loader):0;
  const auto target=Mapped(base,vtable,76)?PPC_LOAD_U32(vtable+72):0;
  REXLOG_INFO("[em1] native NIF factory loader={:08X} vtable={:08X} load={:08X}",loader,vtable,target);
  // From this call onward the stock typed resource path owns/frees the input.
  // Using SystemHeapAlloc here would mismatch the original loader's allocator.
  ctx.r3.u64=manager;ctx.r4.u64=scratch.address;ctx.r5.u64=0x4e494600;
  ctx.r6.u64=data;ctx.r7.u64=closure.model.bytes.size();ctx.r8.u64=closure.model.bytes.size();
  { PublicationScope publication; __imp__sub_8274C588(ctx,base); }
  const auto record=Lookup(ctx,base,manager,scratch.address);
  if(!Mapped(base,record,28)){error="Native model resource was not published";return false;}
  const auto handle=PPC_LOAD_U32(record+20);
  if(texture){
    if(!Mapped(base,handle,16)){error="Native texture handle unavailable";Drop(ctx,base,manager,scratch.address);return false;}
    ctx.r3.u64=scratch.address+272;ctx.r4.u64=handle;__imp__sub_82539F10(ctx,base);
    const auto root=TextureLookup(ctx,base,manager,PPC_LOAD_U32(scratch.address+272),error);
    ctx.r3.u64=scratch.address+272;__imp__sub_822F1C10(ctx,base);
    if(!error.empty() || !Mapped(base,root,0x48) || PPC_LOAD_U32(root)!=0x82041158 || !PPC_LOAD_U32(root+4) ||
        !Mapped(base,handle,16) || !(PPC_LOAD_U32(record+16)&1) || PPC_LOAD_U32(record+12)!=1){
      if(error.empty())error="Native texture factory did not retain a cached NiSourceTexture: root="+std::to_string(root)+
        " table="+std::to_string(Mapped(base,root,4)?PPC_LOAD_U32(root):0);
      Drop(ctx,base,manager,scratch.address);return false;
    }
    output={closure.source,closure.model.origin_key,name,manager,record,handle,root};error.clear();return true;
  }
  // The real NIF loader (832187A8) publishes its root into the loader's named
  // cache, not into this handle. Stock JPSSceneGraphComponent::Load (8279A0F0)
  // then realizes a named handle through manager virtual +36 (shared) or +40
  // (unique). Use that exact shared-resource path; never install a root by hand.
  const auto realize=PPC_LOAD_U32(PPC_LOAD_U32(manager)+36);
  if(!Mapped(base,handle,16) || realize<0x822f0000 || realize>=0x83350000) {
    Drop(ctx,base,manager,scratch.address);error="Native model realization entrypoint is unavailable";return false;
  }
  ctx.r3.u64=manager;ctx.r4.u64=handle;PPC_CALL_INDIRECT_FUNC(realize);
  const bool realized=(ctx.r3.u32&255)!=0;
  const auto root=Mapped(base,handle,16)?PPC_LOAD_U32(handle+12):0;
  if(!realized || !(PPC_LOAD_U32(record+16)&1) || PPC_LOAD_U32(record+12)!=1 || !Mapped(base,root,32)) {
    error="Native model factory did not retain a valid root: flags="+std::to_string(PPC_LOAD_U32(record+16))+" refs="+std::to_string(PPC_LOAD_U32(record+12))+" handle="+std::to_string(handle)+" root="+std::to_string(root);
    Drop(ctx,base,manager,scratch.address);return false;
  }
  output={closure.source,closure.model.origin_key,name,manager,record,handle,root};
  error.clear();return true;
}

bool PublishNativeModel(PPCContext& ctx,uint8_t* base,uint32_t manager,const ModelClosure& closure,
    NativeModelResource& output,std::string& error,std::string_view reserved_name){
  return PublishNativeNif(ctx,base,manager,closure,output,error,reserved_name,false);
}
bool PublishNativeTexture(PPCContext& ctx,uint8_t* base,uint32_t manager,const ModelClosure& closure,
    NativeTextureResource& output,std::string& error){
  return PublishNativeNif(ctx,base,manager,closure,output.owner,error,{},true);
}

bool ReleaseNativeTexture(PPCContext& ctx,uint8_t* base,NativeTextureResource& resource,std::string& error){
  if(!resource.owner.record){error.clear();return true;}
  const auto manager=resource.owner.manager;const auto handle=resource.owner.handle;
  Restore restore(ctx);auto* kernel=rex::kernel::kernel_state();
  try{Require(kernel && kernel->memory(),"Native memory is unavailable");Prepare(ctx,base,manager);Require(Mapped(base,handle,16),"Texture release handle unavailable");}
  catch(const std::exception& e){error=e.what();return false;}
  Names scratch(*kernel->memory());
  if(!scratch.address || !Mapped(base,scratch.address,512,true)){error="Texture release observation scratch unavailable";return false;}
  std::memset(base+scratch.address,0,512);
  ctx.r3.u64=scratch.address+272;ctx.r4.u64=handle;__imp__sub_82539F10(ctx,base);
  const auto key=PPC_LOAD_U32(scratch.address+272);
  const bool released=ReleaseNativeModel(ctx,base,resource.owner,error);
  const auto remaining=released?TextureLookup(ctx,base,manager,key,error):0;
  ctx.r3.u64=scratch.address+272;__imp__sub_822F1C10(ctx,base);
  if(!released)return false;
  if(!error.empty())return false;
  if(remaining){error="Released native texture remains cached";return false;}
  return true;
}

bool ReleaseNativeModel(PPCContext& ctx,uint8_t* base,NativeModelResource& resource,std::string& error) {
  Restore restore(ctx);
  if(!resource.record){error.clear();return true;}
  auto* kernel=rex::kernel::kernel_state();
  try {
    Require(kernel && kernel->memory(),"Native memory is unavailable");
    Require(resource.native_name.starts_with("em1/runtime/") && resource.native_name.size()<240,"Invalid native EM1 resource identity");
    Prepare(ctx,base,resource.manager);
  }catch(const std::exception& e){error=e.what();return false;}
  Names scratch(*kernel->memory());
  if(!scratch.address || !Mapped(base,scratch.address,512,true)){error="Native release name allocation failed";return false;}
  std::memset(base+scratch.address,0,512);
  std::memcpy(base+scratch.address,resource.native_name.c_str(),resource.native_name.size()+1);
  const auto record=Lookup(ctx,base,resource.manager,scratch.address);
  if(record!=resource.record || !Mapped(base,record,28) || PPC_LOAD_U32(record+20)!=resource.handle ||
      !PPC_LOAD_U32(record+12)) {error="Native resource owner changed before release";return false;}
  Drop(ctx,base,resource.manager,scratch.address);
  resource={};error.clear();return true;
}

bool RetainNativeRawScene(PPCContext& ctx,uint8_t* base,uint32_t manager,
    std::shared_ptr<RvzSource> source,std::string_view origin,std::string_view name,
    std::span<const uint8_t> expected,NativeRawSceneResource& output,std::string& error){
  Restore restore(ctx);auto* kernel=rex::kernel::kernel_state();
  try{
    Require(!output.owner.record && source && !origin.empty(),"Invalid raw scene retention owner");
    Require(kernel && kernel->memory(),"Native memory is unavailable");
    Require(name.starts_with("em1/runtime/") && name.ends_with(".bin") && name.size()<240 &&
        name.find("..") == std::string_view::npos && name.find(char(0)) == std::string_view::npos,
        "Invalid retained scene name");
    Require(!expected.empty() && expected.size()<=16*1024*1024,"Invalid retained scene size");
    Prepare(ctx,base,manager);
  }catch(const std::exception& e){error=e.what();return false;}
  Names scratch(*kernel->memory());
  if(!scratch.address || !Mapped(base,scratch.address,512,true)){error="Retained scene name allocation failed";return false;}
  std::memset(base+scratch.address,0,512);std::memcpy(base+scratch.address,name.data(),name.size());
  const auto record=Lookup(ctx,base,manager,scratch.address);
  if(!Mapped(base,record,28) || (PPC_LOAD_U32(record+16)&3) || PPC_LOAD_U32(record+8)!=expected.size()){
    error="Native archive has not published the expected uncompressed raw scene";return false;
  }
  const auto data=PPC_LOAD_U32(record+24),refs=PPC_LOAD_U32(record+12),handle=PPC_LOAD_U32(record+20);
  if(!refs || refs==UINT32_MAX || handle || !Mapped(base,data,expected.size()) ||
      std::memcmp(base+data,expected.data(),expected.size())){
    error="Native scene archive bytes/ownership differ from the original-source adapter";return false;
  }
  // C588's existing-record branch only increments +12 and does not consume
  // an input buffer. Retain the real package record without copying its data.
  ctx.r3.u64=manager;ctx.r4.u64=scratch.address;ctx.r5.u64=0;
  ctx.r6.u64=0;ctx.r7.u64=0;ctx.r8.u64=0;__imp__sub_8274C588(ctx,base);
  if(PPC_LOAD_U32(record+12)!=refs+1){error="Native raw resource retention failed";return false;}
  output.owner={std::move(source),std::string(origin),std::string(name),manager,record,handle,0};
  output.data=data;error.clear();return true;
}

bool PublishNativeRawScene(PPCContext& ctx,uint8_t* base,uint32_t manager,
    std::shared_ptr<RvzSource> source,std::string_view origin,std::string_view name,
    std::span<const uint8_t> bytes,NativeRawSceneResource& output,std::string& error) {
  Restore restore(ctx);
  auto* kernel=rex::kernel::kernel_state();
  try {
    Require(!output.owner.record && source && !origin.empty(),"Invalid raw scene ownership");
    Require(kernel && kernel->memory(),"Native memory is unavailable");
    Require(name.starts_with("em1/runtime/") && name.ends_with(".bin") && name.size()<240 &&
        name.find("..") == std::string_view::npos && name.find(char(0)) == std::string_view::npos,
        "Invalid origin-owned scene name");
    Require(!bytes.empty() && bytes.size()<=16*1024*1024,"Invalid bounded scene bytes");
    Prepare(ctx,base,manager);
  }catch(const std::exception& e){error=e.what();return false;}
  Names scratch(*kernel->memory());
  if(!scratch.address || !Mapped(base,scratch.address,512,true)){error="Scene name allocation failed";return false;}
  std::memset(base+scratch.address,0,512);
  std::memcpy(base+scratch.address,name.data(),name.size());
  if(Lookup(ctx,base,manager,scratch.address)){error="Native scene name already exists";return false;}
  ctx.r3.u64=bytes.size();ctx.r4.u64=32;ctx.r5.u64=0;
  __imp__sub_826549B8(ctx,base);const auto data=ctx.r3.u32;
  if(!data){error="Game scene allocation failed";return false;}
  if(!Mapped(base,data,bytes.size(),true)){
    ctx.r3.u64=data;__imp__sub_82655930(ctx,base);error="Scene allocation is not writable";return false;
  }
  std::memcpy(base+data,bytes.data(),bytes.size());
  // Kind zero is the native raw GSA resource. Its data remains manager-owned;
  // 82749E50 borrows this pointer and does not increment its reference count.
  ctx.r3.u64=manager;ctx.r4.u64=scratch.address;ctx.r5.u64=0;
  ctx.r6.u64=data;ctx.r7.u64=bytes.size();ctx.r8.u64=bytes.size();
  __imp__sub_8274C588(ctx,base);
  const auto record=Lookup(ctx,base,manager,scratch.address);
  if(!Mapped(base,record,28)){error="Raw scene publication produced no record";return false;}
  const auto handle=PPC_LOAD_U32(record+20);
  output.owner={std::move(source),std::string(origin),std::string(name),manager,record,handle,0};
  output.data=PPC_LOAD_U32(record+24);
  if((PPC_LOAD_U32(record+16)&3) || PPC_LOAD_U32(record+8)!=bytes.size() ||
      PPC_LOAD_U32(record+12)!=1 || output.data!=data){
    error="Raw scene record has unexpected ownership or encoding";return false;
  }
  error.clear();return true;
}
bool ReleaseNativeRawScene(PPCContext& ctx,uint8_t* base,NativeRawSceneResource& resource,std::string& error){
  if(!ReleaseNativeModel(ctx,base,resource.owner,error))return false;
  resource={};return true;
}

bool PublishNativeHavok(PPCContext& ctx,uint8_t* base,uint32_t manager,
    std::shared_ptr<RvzSource> source,std::string_view origin,std::span<const uint8_t> bytes,
    NativeHavokResource& output,std::string& error){
  Restore restore(ctx);auto* kernel=rex::kernel::kernel_state();
  try{
    Require(!output.owner.record && !output.wrapper && source && !origin.empty(),"Invalid HKX resource owner");
    Require(kernel && kernel->memory(),"Native memory unavailable");
    Require(bytes.size()>=208 && bytes.size()<=64*1024*1024 && bytes[15]==9 &&
        std::memcmp(bytes.data()+40,"hk_2011.2.0-r1",13)==0,"HKX publication requires a converted native pack");
    Prepare(ctx,base,manager);
  }catch(const std::exception& e){error=e.what();return false;}
  Names scratch(*kernel->memory());if(!scratch.address || !Mapped(base,scratch.address,512,true)){error="HKX name allocation failed";return false;}
  const auto name="em1/runtime/"+std::to_string(next_name.fetch_add(1))+".hkx";
  std::memset(base+scratch.address,0,512);std::memcpy(base+scratch.address,name.c_str(),name.size()+1);
  if(Lookup(ctx,base,manager,scratch.address)){error="HKX name collision";return false;}
  ctx.r3.u64=manager;ctx.r4.u64=0x484B5800;__imp__sub_83089FE0(ctx,base);
  const auto loader=ctx.r3.u32,table=Mapped(base,loader,4)?PPC_LOAD_U32(loader):0;
  if(!Mapped(base,table,76) || PPC_LOAD_U32(table+72)!=0x83219770){error="HKX factory differs from audited loader";return false;}
  ctx.r3.u64=bytes.size();ctx.r4.u64=32;ctx.r5.u64=0;__imp__sub_826549B8(ctx,base);const auto data=ctx.r3.u32;
  if(!data){error="HKX game allocation failed";return false;}
  if(!Mapped(base,data,bytes.size(),true)){ctx.r3.u64=data;__imp__sub_82655930(ctx,base);error="HKX game allocation inaccessible";return false;}
  std::memcpy(base+data,bytes.data(),bytes.size());
  // Stock publication owns the input from here. Its HKX wrapper performs
  // in-place load/unload; neither the root nor input may be freed separately.
  ctx.r3.u64=manager;ctx.r4.u64=scratch.address;ctx.r5.u64=0x484B5800;
  ctx.r6.u64=data;ctx.r7.u64=bytes.size();ctx.r8.u64=bytes.size();__imp__sub_8274C588(ctx,base);
  const auto record=Lookup(ctx,base,manager,scratch.address);
  if(!Mapped(base,record,28)){error="Native HKX publication missing";return false;}
  output.owner={std::move(source),std::string(origin),name,manager,record,PPC_LOAD_U32(record+20),0};
  // 83219080 checks the typed handle's flags (+4) and interns its name (+8).
  // Its argument is not the raw filename accepted by manager publication.
  if(!Mapped(base,output.owner.handle,16)){error="Native HKX handle unavailable";return false;}
  ctx.r3.u64=loader;ctx.r4.u64=output.owner.handle;ctx.r5.u64=scratch.address+272;
  __imp__sub_83219080(ctx,base);output.wrapper=PPC_LOAD_U32(scratch.address+272);
  if(!ctx.r3.u32 || !Mapped(base,output.wrapper,32)){error="Native HKX cache did not return a retained wrapper";return false;}
  output.owner.root=PPC_LOAD_U32(output.wrapper+20);
  if(!Mapped(base,output.owner.root,16) || PPC_LOAD_U32(record+12)!=1 || !PPC_LOAD_U16(output.wrapper+6)){
    error="Native HKX ownership validation failed";return false;
  }
  REXLOG_INFO("[em1] native HKX published name='{}' wrapper={:08X} root={:08X} wrapper_refs={}",name,output.wrapper,output.owner.root,PPC_LOAD_U16(output.wrapper+6));
  error.clear();return true;
}
bool ReleaseNativeHavok(PPCContext& ctx,uint8_t* base,NativeHavokResource& resource,std::string& error){
  Restore restore(ctx);
  if(resource.wrapper){
    try{Prepare(ctx,base,resource.owner.manager);Require(Mapped(base,resource.wrapper,32) && PPC_LOAD_U16(resource.wrapper+6),"Native HKX wrapper unavailable");}
    catch(const std::exception& e){error=e.what();return false;}
    ctx.r3.u64=resource.wrapper;__imp__sub_8238C448(ctx,base);resource.wrapper=0;
  }
  if(!ReleaseNativeModel(ctx,base,resource.owner,error))return false;
  resource={};return true;
}

}
