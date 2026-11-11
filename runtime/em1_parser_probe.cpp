#include "em1_parser_probe.h"
#include "em1_scene_adapter.h"
#include "em1_scene_files.h"
#include "em1_native_component.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <functional>
#include <stdexcept>
#include <rex/logging.h>

PPC_EXTERN_FUNC(__imp__sub_827A8C40);  // constructor; caller owns outer allocation
PPC_EXTERN_FUNC(__imp__sub_827A8908);  // destructor; releases members, not self
PPC_EXTERN_FUNC(__imp__sub_827A8AF8);  // unload named scene template ownership before parser destruction
PPC_EXTERN_FUNC(__imp__sub_827A9458);  // begin, optionally deferred
PPC_EXTERN_FUNC(__imp__sub_827A8F48);  // pump; r5 is a MICROSECOND budget
PPC_EXTERN_FUNC(__imp__sub_826547C8); // matching raw parser output-vector free
PPC_EXTERN_FUNC(__imp__sub_827BA550); // original entity blueprint binary reader
PPC_EXTERN_FUNC(__imp__sub_827B7E80); // retain entity blueprint
PPC_EXTERN_FUNC(__imp__sub_827BA728); // release entity blueprint and owned data
PPC_EXTERN_FUNC(__imp__sub_827B96B0); // inherit component definitions; retains assigned parent
PPC_EXTERN_FUNC(sub_827A9458);
PPC_EXTERN_FUNC(sub_827A8F48);
PPC_EXTERN_FUNC(sub_827A8908);
PPC_EXTERN_FUNC(__imp__sub_825430D8);

namespace em2::em1 {
namespace {
thread_local bool constructing=false;
using BlueprintGuid=std::array<uint32_t,4>;
thread_local const std::map<BlueprintGuid,uint32_t>* parent_scope=nullptr;
thread_local std::atomic<uint64_t>* parent_resolutions=nullptr;
bool Writable(uint8_t* base, uint32_t start, size_t bytes) {
  if (!base || start < 4096 || !bytes || uint64_t(start) + bytes > 0x100000000ull)
    return false;
  auto at = reinterpret_cast<uintptr_t>(base + start);
  const auto end = at + bytes;
  while (at < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
      return false;
    const auto access = info.Protect & 0xff;
    if (access != PAGE_READWRITE && access != PAGE_WRITECOPY &&
        access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY)
      return false;
    const auto next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (next <= at) return false;
    at = std::min(end, next);
  }
  return true;
}
struct ContextScope {
  PPCContext& ctx;
  PPCContext saved;
  unsigned csr;
  explicit ContextScope(PPCContext& context)
      : ctx(context), saved(context), csr(_mm_getcsr()) {
    // Preserve the interrupted caller's ABI home slots. The finalizer itself
    // uses a 12,208-byte guest frame, hence the larger mapping check below.
    ctx.r1.u64 -= 2048;
  }
  ~ContextScope() { ctx = saved; _mm_setcsr(csr); }
};
}

bool NativeSceneConstructionActive(){return constructing;}
NativeBlueprintParentScope::NativeBlueprintParentScope(const NativeBlueprintIndex* index,std::atomic<uint64_t>* counter)
  :previous_(parent_scope),previous_counter_(parent_resolutions){parent_scope=index;parent_resolutions=counter;}
NativeBlueprintParentScope::~NativeBlueprintParentScope(){parent_scope=previous_;parent_resolutions=previous_counter_;}
void ResolveNativeBlueprintParent(PPCContext& ctx,uint8_t* base){
  if(!parent_scope)return;
  const auto entity=ctx.r3.u32;
  if(!Writable(base,entity,0x48))throw std::runtime_error("Original blueprint parent resolution received an invalid entity");
  BlueprintGuid master{};for(unsigned i=0;i<4;++i)master[i]=PPC_LOAD_U32(entity+0x14+i*4);
  if(master==BlueprintGuid{})return;
  const auto found=parent_scope->find(master);
  if(found==parent_scope->end())throw std::runtime_error("Original blueprint master is absent from its retained source level scope");
  if(!PPC_LOAD_U32(entity+0x38))PPC_STORE_U32(entity+0x38,found->second);
  if(parent_resolutions)parent_resolutions->fetch_add(1,std::memory_order_relaxed);
}

bool ReadNativeComponentFactories(uint8_t* base,std::map<std::string,uint32_t>& output,std::string& error){
  struct Restore{DWORD last=GetLastError();unsigned csr=_mm_getcsr();~Restore(){_mm_setcsr(csr);SetLastError(last);}} restore;
  auto fail=[&](const char* reason){error=reason;return false;};
  auto read=[&](uint64_t at,void* out,size_t size){SIZE_T n=0;return base && at>=4096 && at<=0x100000000ull && size<=0x100000000ull-at && ReadProcessMemory(GetCurrentProcess(),base+at,out,size,&n) && n==size;};
  auto word=[&](uint64_t at,uint32_t& value){uint32_t raw=0;if(!read(at,&raw,4))return false;value=_byteswap_ulong(raw);return true;};
  uint32_t factory=0,map=0,count=0,buckets=0,entries=0;
  // 82503FC0 returns this factory. 82510678 reads its map at+8 and
  // 825396B8 reads bucket count+4, array+8 and node{next,key,callback}.
  if(!word(0x833CD7A0,factory) || !word(uint64_t(factory)+8,map) ||
      !word(uint64_t(map)+4,count) || !word(uint64_t(map)+8,buckets) ||
      !word(uint64_t(map)+12,entries) || !count || count>65536 || entries>8192)
    return fail("Native component creation map unavailable");
  std::map<std::string,uint32_t> result;std::set<uint32_t> visited;
  for(uint32_t i=0;i<count;++i){uint32_t node=0;
    if(!word(uint64_t(buckets)+uint64_t(i)*4,node))return fail("Unreadable native creation bucket");
    while(node){uint32_t next=0,key=0,callback=0;
      if(visited.size()>=8192 || !visited.insert(node).second || !word(node,next) ||
          !word(uint64_t(node)+4,key) || !word(uint64_t(node)+8,callback) || key%count!=i || (callback&3))
        return fail("Invalid native creation map chain");
      std::string name;bool ended=false;
      for(unsigned j=0;j<256;++j){char ch=0;if(!read(uint64_t(key)+j,&ch,1))return fail("Unreadable native creation class name");
        if(!ch){ended=true;break;}if(ch<32 || ch>126)return fail("Invalid native creation class name");name.push_back(ch);}
      if(!ended || name.empty() || !result.emplace(name,callback).second)return fail("Invalid or duplicate native creation class");
      node=next;
    }
  }
  uint32_t after_factory=0,after_map=0,after_count=0,after_buckets=0,after_entries=0;
  if(visited.size()!=entries || !word(0x833CD7A0,after_factory) || after_factory!=factory ||
      !word(uint64_t(factory)+8,after_map) || after_map!=map || !word(uint64_t(map)+4,after_count) || after_count!=count ||
      !word(uint64_t(map)+8,after_buckets) || after_buckets!=buckets || !word(uint64_t(map)+12,after_entries) || after_entries!=entries)
    return fail("Native creation map changed during inspection");
  output=std::move(result);error.clear();return true;
}
bool ProbeNativeSceneParser(PPCContext& ctx, uint8_t* base, uint32_t scratch,
                            size_t scratch_size, std::string& error) {
  error.clear();
  constexpr uint32_t stack_window = 32768;
  if (scratch_size < kParserProbeScratchSize || (scratch & 31) ||
      !Writable(base, scratch, kParserProbeScratchSize) || ctx.r1.u32 < stack_window ||
      !Writable(base, ctx.r1.u32 - stack_window, stack_window) ||
      (uint64_t(scratch) < ctx.r1.u32 &&
       uint64_t(scratch) + kParserProbeScratchSize > ctx.r1.u32 - stack_window)) {
    error = "Parser probe requires private aligned scratch and 32 KiB of writable guest stack";
    return false;
  }
  if (!std::all_of(base + scratch, base + scratch + kParserProbeScratchSize,
                   [](uint8_t value) { return value == 0; })) {
    error = "Parser probe refuses nonzero scratch; never pass a live engine object";
    return false;
  }
  std::array<uint8_t, 28> original{};
  original[3] = 4;  // empty EM1 string table, UUID, entity count, reference count
  SceneAdapterResult adapted;
  if (!AdaptScene(original, adapted, error)) return false;
  if (adapted.bytes.size() != 24 || adapted.entity_count || adapted.component_count) {
    error = "Unexpected synthetic EM1 adapter result";
    return false;
  }
  // The real class ends at the fixed-string member +164. Reserve 512 bytes
  // conservatively; input, dependency vector, and name reference are disjoint.
  const uint32_t owner = scratch, input = scratch + 512;
  const uint32_t dependencies = scratch + 576, name = scratch + 608;
  ContextScope context(ctx);
  for (bool cancel : {false, true}) {
    std::memset(base + scratch, 0, kParserProbeScratchSize);
    std::memcpy(base + input, adapted.bytes.data(), adapted.bytes.size());
    ctx.r3.u64 = owner;
    __imp__sub_827A8C40(ctx, base);
    // All subsequent checks lead through the complete object's destructor.
    // Unexpected guest exceptions intentionally propagate; a partially built
    // object cannot safely be guessed at or freed as though it were complete.
    bool valid = ctx.r3.u32 == owner && base[owner + 124] == 0;
    if (valid) {
      ctx.r3.u64 = owner; ctx.r4.u64 = input;
      ctx.r5.u64 = adapted.bytes.size(); ctx.r6.u64 = 0;
      ctx.r7.u64 = 0; ctx.r8.u64 = 0;
      ctx.r9.u64 = name; ctx.r10.u64 = dependencies;
      __imp__sub_827A9458(ctx, base);
      valid = ctx.r3.u32 == 1 && base[owner + 124] == 1 &&
              PPC_LOAD_U32(owner + 112) == input &&
              PPC_LOAD_U32(owner + 104) == 0 && PPC_LOAD_U32(owner + 108) == 0 &&
              PPC_LOAD_U32(owner + 116) == adapted.bytes.size();
    }
    if (valid && !cancel) {
      ctx.r3.u64 = owner; ctx.r4.u64 = 0; ctx.r5.u64 = 0;
      __imp__sub_827A8F48(ctx, base);
      valid = ctx.r3.u32 == 1 && base[owner + 124] == 0;
    }
    // On the cancellation pass the retained input remains allocated throughout
    // destruction. No live parser/global busy flag is cleared by this fixture.
    ctx.r3.u64 = owner;
    __imp__sub_827A8908(ctx, base);
    valid = valid && base[owner + 124] == 0 &&
            PPC_LOAD_U32(dependencies) == 0 && PPC_LOAD_U32(dependencies + 4) == 0 &&
            PPC_LOAD_U32(owner + 16) == 0 && PPC_LOAD_U32(owner + 28) == 0 &&
            PPC_LOAD_U32(owner + 52) == 0 && PPC_LOAD_U32(owner + 144) == 0;
    std::memset(base + scratch, 0, kParserProbeScratchSize);
    if (!valid) {
      error = cancel ? "Isolated native parser cancellation contract failed"
                     : "Isolated native parser deferred/completion contract failed";
      return false;
    }
  }
  return true;
}

static bool ReadSceneBlueprintDefinitions(PPCContext& ctx,uint8_t* base,uint32_t scratch,size_t scratch_size,
    const SceneAdapterResult& scene,NativeBlueprintReport& output,std::string& error,std::vector<uint32_t>* retained,bool inspect_factories){
  std::vector<SceneFileReference> references;if(!CollectSceneFileReferences(scene,references,error))return false;
  if(scene.bytes.size()>64*1024*1024 || scene.entity_count>10000 || scratch_size<kParserProbeScratchSize+scene.bytes.size() ||
      !Writable(base,scratch,scratch_size) || ctx.r1.u32<32768 || !Writable(base,ctx.r1.u32-32768,32768) ||
      (uint64_t(scratch)<ctx.r1.u32 && uint64_t(scratch)+scratch_size>ctx.r1.u32-32768)){
    error="Complete blueprint reader requires private scratch and engine stack";return false;}
  auto word=[&](size_t at){return uint32_t(scene.bytes.at(at))<<24|uint32_t(scene.bytes.at(at+1))<<16|uint32_t(scene.bytes.at(at+2))<<8|scene.bytes.at(at+3);};
  const auto header=word(4);const auto placements=word(size_t(header)+16);
  size_t cursor=size_t(header)+20,footer=scene.bytes.size()-size_t(placements)*4;
  const auto input=scratch+uint32_t(kParserProbeScratchSize);std::memcpy(base+input,scene.bytes.data(),scene.bytes.size());
  if(retained)retained->reserve(retained->size()+scene.entity_count);
  std::vector<uint32_t> definitions;definitions.reserve(scene.entity_count);ContextScope restore(ctx);NativeBlueprintReport report;bool valid=true;
  for(uint32_t i=0;i<scene.entity_count && valid;++i){
    ctx.r3.u64=input+uint32_t(cursor);ctx.r4.u64=input+4;__imp__sub_827BA550(ctx,base);const auto entity=ctx.r3.u32;
    if(!Writable(base,entity,0x48)){error="Native blueprint reader returned no entity definition";valid=false;break;}
    ctx.r3.u64=entity;__imp__sub_827B7E80(ctx,base);definitions.push_back(entity);
    const auto consumed=PPC_LOAD_U32(entity+0x3C),components=PPC_LOAD_U32(entity+0x30),array=PPC_LOAD_U32(entity+0x2C);
    if(!consumed || cursor>footer || consumed>footer-cursor || components>4096 ||
        (components && !Writable(base,array,size_t(components)*4)) || PPC_LOAD_U32(entity+0x38) || PPC_LOAD_U32(entity+0x44)){
      error="Native blueprint size/component/unlinked-state check failed";valid=false;break;}
    cursor+=consumed;++report.entities;report.components+=components;
    for(uint32_t j=0;j<components;++j){const auto component=PPC_LOAD_U32(array+j*4);
      if(!Writable(base,component,0x38)){error="Invalid native component definition";valid=false;break;}
      report.properties+=PPC_LOAD_U32(component+0x28);
    }
  }
  valid=valid && cursor==footer && report.entities==scene.entity_count && report.components==scene.component_count;
  if(valid && inspect_factories)for(const auto entity:definitions){const auto count=PPC_LOAD_U32(entity+0x30),array=PPC_LOAD_U32(entity+0x2C);
    for(uint32_t i=0;i<count;++i){const auto component=PPC_LOAD_U32(array+i*4),name=PPC_LOAD_U32(component);
      if(PPC_LOAD_U32(component+0x28)==17 && Writable(base,name,19) && !std::memcmp(base+name,"InventoryComponent",19)){
        if(!ProbeNativeInventoryFactory(ctx,base,error,component)){valid=false;break;}++report.authored_inventory_probes;
      }
      if(PPC_LOAD_U32(component+0x28)!=14 || PPC_LOAD_U32(component+0x14) || PPC_LOAD_U32(component+0x18) || PPC_LOAD_U32(component+0x1C) || PPC_LOAD_U32(component+0x20))continue;
      if(!Writable(base,name,22) || std::memcmp(base+name,"AmmoRechargeComponent",22))continue;
      if(!ProbeNativeAmmoComponent(ctx,base,error,component)){valid=false;break;}++report.authored_ammo_probes;
    }
    if(!valid)break;
  }
  const bool unchanged=std::memcmp(base+input,scene.bytes.data(),scene.bytes.size())==0;
  if(valid && unchanged && retained){retained->insert(retained->end(),definitions.begin(),definitions.end());definitions.clear();}
  for(auto at=definitions.rbegin();at!=definitions.rend();++at){ctx.r3.u64=*at;__imp__sub_827BA728(ctx,base);}
  if(!valid || !unchanged){if(error.empty())error="Complete native blueprint counts/footer/source preservation failed";return false;}
  output=report;error.clear();return true;
}
bool ProbeNativeSceneBlueprints(PPCContext& ctx,uint8_t* base,uint32_t scratch,size_t size,
    const SceneAdapterResult& scene,NativeBlueprintReport& output,std::string& error,std::vector<uint32_t>* retained){
  return ReadSceneBlueprintDefinitions(ctx,base,scratch,size,scene,output,error,retained,true);
}
bool ReadNativeSceneBlueprintDefinitions(PPCContext& ctx,uint8_t* base,uint32_t scratch,size_t size,
    const SceneAdapterResult& scene,NativeBlueprintReport& output,std::string& error,std::vector<uint32_t>& retained){
  return ReadSceneBlueprintDefinitions(ctx,base,scratch,size,scene,output,error,&retained,false);
}

void ReleaseNativeBlueprints(PPCContext& ctx,uint8_t* base,std::vector<uint32_t>& definitions){
  ContextScope restore(ctx);
  for(auto at=definitions.rbegin();at!=definitions.rend();++at){ctx.r3.u64=*at;__imp__sub_827BA728(ctx,base);}
  definitions.clear();
}

bool LinkNativeBlueprintInheritance(PPCContext& ctx,uint8_t* base,std::span<const uint32_t> definitions,
    NativeInheritanceReport& output,std::string& error){
  using Guid=std::array<uint32_t,4>;
  auto guid=[&](uint32_t at){return Guid{PPC_LOAD_U32(at),PPC_LOAD_U32(at+4),PPC_LOAD_U32(at+8),PPC_LOAD_U32(at+12)};};
  if(definitions.empty() || definitions.size()>10000 || ctx.r1.u32<32768 || !Writable(base,ctx.r1.u32-32768,32768)){
    error="Scoped inheritance requires retained definitions and an engine stack";return false;}
  std::map<Guid,size_t> index;std::vector<size_t> parents(definitions.size(),definitions.size()),order;
  std::vector<uint8_t> state(definitions.size());std::vector<uint32_t> expected_refs(definitions.size(),1);NativeInheritanceReport report;
  for(size_t i=0;i<definitions.size();++i){const auto entity=definitions[i];
    if(!Writable(base,entity,0x48) || PPC_LOAD_U32(entity+0x38) || PPC_LOAD_U32(entity+0x40)!=1 || PPC_LOAD_U32(entity+0x44)){
      error="Scoped inheritance requires exclusively retained, unlinked data definitions";return false;}
    if(index.emplace(guid(entity+4),i).second)++report.unique;else ++report.duplicates;
    const auto count=PPC_LOAD_U32(entity+0x30),array=PPC_LOAD_U32(entity+0x2C);
    if(count>4096 || (count && !Writable(base,array,size_t(count)*4))){error="Invalid scoped component vector";return false;}
    for(uint32_t c=0;c<count;++c){const auto component=PPC_LOAD_U32(array+c*4);
      if(!Writable(base,component,0x38) || PPC_LOAD_U32(component+0x30)){error="Invalid/already linked scoped component";return false;}}
  }
  for(size_t i=0;i<definitions.size();++i){const auto master=guid(definitions[i]+0x14);if(master==Guid{})continue;
    const auto found=index.find(master);if(found==index.end()){error="Entity master is absent from the selected original level scope";return false;}
    parents[i]=found->second;++expected_refs[found->second];++report.parents;
  }
  std::function<bool(size_t,unsigned)> visit=[&](size_t i,unsigned depth){
    if(state[i]==2)return true;if(state[i]==1 || depth>512)return false;state[i]=1;
    if(parents[i]!=definitions.size() && !visit(parents[i],depth+1))return false;
    state[i]=2;order.push_back(i);return true;};
  for(size_t i=0;i<definitions.size();++i)if(!visit(i,0)){error="Cyclic or excessive native blueprint inheritance";return false;}
  // Preflight the ENTIRE graph before touching pointers. Supplying the exact
  // scoped parent makes the retail linker bypass its global-manager lookup.
  // Its component clone routine does not use the manager argument.
  ContextScope restore(ctx);
  for(const auto i:order){const auto entity=definitions[i],before=PPC_LOAD_U32(entity+0x30);
    if(parents[i]!=definitions.size()){
      PPC_STORE_U32(entity+0x38,definitions[parents[i]]);ctx.r3.u64=entity;__imp__sub_827B96B0(ctx,base);
    }
    const auto after=PPC_LOAD_U32(entity+0x30);report.components+=after;report.inherited_components+=after-before;
  }
  for(size_t i=0;i<definitions.size();++i){const auto entity=definitions[i];
    if(PPC_LOAD_U32(entity+0x40)!=expected_refs[i]){error="Native inherited definition ownership mismatch";return false;}
    if(parents[i]==definitions.size())continue;
    const auto parent=definitions[parents[i]],parent_count=PPC_LOAD_U32(parent+0x30),parent_array=PPC_LOAD_U32(parent+0x2C);
    const auto count=PPC_LOAD_U32(entity+0x30),array=PPC_LOAD_U32(entity+0x2C);
    for(uint32_t p=0;p<parent_count;++p){const auto source=PPC_LOAD_U32(parent_array+p*4);bool matched=false;
      for(uint32_t c=0;c<count;++c){const auto child=PPC_LOAD_U32(array+c*4);
        if(PPC_LOAD_U32(child)!=PPC_LOAD_U32(source))continue;
        matched=PPC_LOAD_U32(child+0x30)==source;break;
      }
      if(!matched){error="Native inherited component does not reference its scoped parent";return false;}++report.component_links;
    }
  }
  output=report;error.clear();return true;
}

bool ProbeNativeSceneConstruction(PPCContext& ctx,uint8_t* base,uint32_t scratch,size_t scratch_size,
    const SceneAdapterResult& scene,SceneConstructionReport& output,std::string& error,std::span<const uint32_t> source_definitions,bool registered_scope,std::string_view scene_name){
  // The retail parser assumes every authored class has a registered factory.
  // A missing EM1 class instead leaves a null component and crashes during
  // finalization. Reject the entire scene before construction; never remove it.
  std::vector<SceneFileReference> files;std::vector<std::string> classes;
  if(!CollectSceneFileReferences(scene,files,error,false,&classes))return false;
  std::map<std::string,uint32_t> factories;if(!ReadNativeComponentFactories(base,factories,error))return false;
  std::set<std::string> missing;for(const auto& name:classes){const auto found=factories.find(name);if(found==factories.end() || !found->second)missing.insert(name);}
  if(!missing.empty()){error="Complete scene requires unavailable EM1 component creation callbacks:";for(const auto& name:missing)error+=" "+name;return false;}
  error.clear();auto word=[&](size_t at){return uint32_t(scene.bytes.at(at))<<24|uint32_t(scene.bytes.at(at+1))<<16|uint32_t(scene.bytes.at(at+2))<<8|scene.bytes.at(at+3);};
  if(scene.bytes.size()<24 || scene.bytes.size()>64*1024*1024 || scratch_size<kParserProbeScratchSize+scene.bytes.size() ||
      !Writable(base,scratch,scratch_size) || ctx.r1.u32<32768 || !Writable(base,ctx.r1.u32-32768,32768) ||
      (uint64_t(scratch)<ctx.r1.u32 && uint64_t(scratch)+scratch_size>ctx.r1.u32-32768)){
    error="Complete scene construction requires private scratch and writable guest stack";return false;}
  const auto end=uint64_t(word(4))+4;
  if(word(0)!=0x01000001 || end+16>scene.bytes.size() || word(size_t(end))!=0x02000002){error="Invalid complete construction scene";return false;}
  const auto expected=word(size_t(end)+12);
  if(expected>10000 || !std::all_of(base+scratch,base+scratch+kParserProbeScratchSize,[](uint8_t c){return c==0;})){
    error="Complete scene construction budget/nonzero scratch rejected";return false;}
  const auto owner=scratch,input=scratch+uint32_t(kParserProbeScratchSize),vector=scratch+512,name=scratch+544,dependencies=scratch+576;
  std::map<BlueprintGuid,uint32_t> scope;
  for(const auto definition:source_definitions){if(!Writable(base,definition,0x48)){error="Source blueprint scope contains an invalid definition";return false;}
    BlueprintGuid guid{};for(unsigned i=0;i<4;++i)guid[i]=PPC_LOAD_U32(definition+4+i*4);scope.try_emplace(guid,definition);
  }
  struct ParentScope {const std::map<BlueprintGuid,uint32_t>* saved=parent_scope;
    explicit ParentScope(const std::map<BlueprintGuid,uint32_t>& value){if(!value.empty())parent_scope=&value;}
    ~ParentScope(){parent_scope=saved;}} parent_guard(scope);
  const auto navigation_before=NativeNavigationComponents();
  std::memcpy(base+input,scene.bytes.data(),scene.bytes.size());ContextScope restore(ctx);
  ctx.r3.u64=owner;__imp__sub_827A8C40(ctx,base);
  // A scene name lets the retail unload operation find and release the
  // template references retained by the load, before parser destruction.
  if(scene_name.empty())scene_name="em1/runtime/construction_probe.bin";
  if(scene_name.size()>240 || scene_name.find('\0')!=scene_name.npos){error="Invalid constructor scene name";ctx.r3.u64=owner;__imp__sub_827A8908(ctx,base);return false;}
  const auto text=scratch+640;std::memcpy(base+text,scene_name.data(),scene_name.size());base[text+scene_name.size()]=0;
  ctx.r3.u64=text;__imp__sub_825430D8(ctx,base);PPC_STORE_U32(name,ctx.r3.u32);
  ctx.r3.u64=owner;ctx.r4.u64=input;ctx.r5.u64=scene.bytes.size();ctx.r6.u64=vector;ctx.r7.u64=0;ctx.r8.u64=0;ctx.r9.u64=name;ctx.r10.u64=dependencies;
  if(registered_scope)sub_827A9458(ctx,base);else __imp__sub_827A9458(ctx,base);const bool began=ctx.r3.u32==1;
  if(began){struct Scope{bool previous=constructing;Scope(){constructing=true;}~Scope(){constructing=previous;}} scope;
    ctx.r3.u64=owner;ctx.r4.u64=vector;ctx.r5.u64=0;if(registered_scope)sub_827A8F48(ctx,base);else __imp__sub_827A8F48(ctx,base);}
  bool valid=began && ctx.r3.u32==1 && base[owner+124]==0 && PPC_LOAD_U32(vector+4)==expected;
  const auto items=PPC_LOAD_U32(vector);SceneConstructionReport report;report.placements=PPC_LOAD_U32(vector+4);
  std::vector<uint32_t> retained_entities;retained_entities.reserve(report.placements);
  if(valid && report.placements && !Writable(base,items,size_t(report.placements)*4))valid=false;
  for(uint32_t i=0;i<report.placements && valid;++i){
    const auto entity=PPC_LOAD_U32(items+i*4);if(!Writable(base,entity,104)){valid=false;break;}
    const auto face=entity+8;ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(face)));retained_entities.push_back(entity);
    const auto count=PPC_LOAD_U32(entity+52),capacity=PPC_LOAD_U32(entity+48),components=PPC_LOAD_U32(entity+44);
    if(count>256 || count>capacity || (count && !Writable(base,components,size_t(count)*4)) || PPC_LOAD_U32(entity+88)!=0){valid=false;break;}
    report.components+=count;
    for(uint32_t j=0;j<count;++j){const auto component=PPC_LOAD_U32(components+j*4);
      if(component<8 || !Writable(base,component-8,16)){valid=false;break;}
      if(IsNativeNavigationComponent(base,component-8)){NativeNavigationProfile profile;size_t graphs=0;
        if(!ReadNativeNavigationComponent(ctx,base,component-8,profile,graphs,error)){valid=false;break;}
        REXLOG_INFO("[em1] constructed navigation profile: file='{}' pitch={} max={},{},{} min={},{},{} radius={} height={} movement={}",
          profile.filename,profile.pitch,profile.maximum[0],profile.maximum[1],profile.maximum[2],profile.minimum[0],profile.minimum[1],profile.minimum[2],profile.radius,profile.height,profile.movement);
      }
    }
  }
  const bool unchanged=std::memcmp(base+input,scene.bytes.data(),scene.bytes.size())==0;
  const auto navigation=NativeNavigationComponents();
  valid=valid && navigation.instances>=navigation_before.instances && navigation.graphs>=navigation_before.graphs && navigation.failed==navigation_before.failed;
  report.navigation_instances=navigation.instances-navigation_before.instances;report.navigation_graphs=navigation.graphs-navigation_before.graphs;
  // Match the native entry's retain/release pair while inspecting borrowed
  // output pointers. Keep the parser alive through every entity release.
  for(const auto entity:retained_entities){
    const auto face=entity+8;ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(face)+4));
  }
  // Retail unload owns an extra reference for each named prefab. The parser
  // destructor clears its indexes, but does not unload the scene's references.
  // Use the engine's unload operation, rather than editing reference counts.
  if(began){ctx.r3.u64=owner;ctx.r4.u64=owner+164;__imp__sub_827A8AF8(ctx,base);}
  ctx.r3.u64=owner;if(registered_scope)sub_827A8908(ctx,base);else __imp__sub_827A8908(ctx,base);
  if(items){ctx.r3.u64=items;__imp__sub_826547C8(ctx,base);}
  const bool released=base[owner+124]==0 && PPC_LOAD_U32(owner+16)==0 && PPC_LOAD_U32(owner+28)==0 && PPC_LOAD_U32(owner+52)==0 && PPC_LOAD_U32(owner+144)==0 && NativeNavigationComponents()==navigation_before;
  if(!valid || !unchanged || !released){output=report;error="Complete scene construction/count/component/cleanup check failed: began="+std::to_string(began)+" placements="+std::to_string(report.placements)+" components="+std::to_string(report.components)+" expected="+std::to_string(expected)+" valid="+std::to_string(valid)+" navigation_failed="+std::to_string(navigation.failed)+" unchanged="+std::to_string(unchanged)+" released="+std::to_string(released);return false;}
  output=report;error.clear();return true;
}
bool ProbeNativeSceneHeader(PPCContext& ctx, uint8_t* base, uint32_t scratch,
                            size_t scratch_size, const SceneAdapterResult& scene,
                            std::string& error) {
  error.clear();
  const auto word = [&](size_t at) {
    return uint32_t(scene.bytes[at]) << 24 | uint32_t(scene.bytes[at+1]) << 16 |
           uint32_t(scene.bytes[at+2]) << 8 | scene.bytes[at+3];
  };
  if (scene.bytes.size() < 24 || scene.bytes.size() > 64 * 1024 * 1024 ||
      word(0) != 0x01000001) {
    error = "Native header probe requires a validated adapted EM1 scene"; return false;
  }
  const size_t string_end = word(4);
  if (string_end < 4 || string_end > scene.bytes.size() - 20 ||
      word(string_end + 4) != 0x02000002 || word(string_end + 8) != 0 ||
      word(string_end + 12) != scene.entity_count) {
    error = "Unexpected adapted EM1 scene header/dependencies"; return false;
  }
  const size_t required = kParserProbeScratchSize + scene.bytes.size();
  constexpr uint32_t stack_window = 32768;
  if (scratch_size < required || (scratch & 31) || !Writable(base,scratch,required) ||
      ctx.r1.u32 < stack_window ||
      !Writable(base,ctx.r1.u32-stack_window,stack_window) ||
      (uint64_t(scratch) < ctx.r1.u32 && uint64_t(scratch)+required > ctx.r1.u32-stack_window) ||
      !std::all_of(base+scratch,base+scratch+required,[](uint8_t v){return v==0;})) {
    error = "Native header probe requires private zero scratch and writable guest stack"; return false;
  }
  const uint32_t owner=scratch, input=scratch+uint32_t(kParserProbeScratchSize);
  const uint32_t dependencies=scratch+576, name=scratch+608;
  std::memcpy(base+input,scene.bytes.data(),scene.bytes.size());
  ContextScope context(ctx);
  ctx.r3.u64=owner; __imp__sub_827A8C40(ctx,base);
  bool valid=ctx.r3.u32==owner && base[owner+124]==0;
  if (valid) {
    ctx.r3.u64=owner; ctx.r4.u64=input; ctx.r5.u64=scene.bytes.size();
    ctx.r6.u64=0; ctx.r7.u64=0; ctx.r8.u64=0;
    ctx.r9.u64=name; ctx.r10.u64=dependencies;
    __imp__sub_827A9458(ctx,base);
    valid=ctx.r3.u32==1 && base[owner+124]==1 &&
          PPC_LOAD_U32(owner+112)==input && PPC_LOAD_U32(owner+116)==string_end+20 &&
          PPC_LOAD_U32(owner+120)==input+4 &&
          PPC_LOAD_U32(owner+104)==scene.entity_count &&
          PPC_LOAD_U32(owner+108)==word(string_end+16) &&
          PPC_LOAD_U32(owner+72)==0 && PPC_LOAD_U32(owner+80)==0 &&
          PPC_LOAD_U32(owner+88)==0 && PPC_LOAD_U32(owner+96)==0;
  }
  // Deliberately NO pump: actual component types, meshes, Lua, and Havok are
  // not activated until their independent compatibility gates are complete.
  ctx.r3.u64=owner; __imp__sub_827A8908(ctx,base);
  valid=valid && base[owner+124]==0 && PPC_LOAD_U32(owner+16)==0 &&
        PPC_LOAD_U32(owner+28)==0 && PPC_LOAD_U32(owner+52)==0 &&
        PPC_LOAD_U32(owner+144)==0 && PPC_LOAD_U32(dependencies)==0 &&
        std::memcmp(base+input,scene.bytes.data(),scene.bytes.size())==0;
  std::memset(base+scratch,0,kParserProbeScratchSize);
  if (!valid) error="Actual EM1 scene header retention/cancellation contract failed";
  return valid;
}

}
