#include "em1_model_validation.h"
#include "em1_native_resources.h"
#include "em1_havok_collision.h"
#include "em1_havok_collision_probe.h"
#include "em1_havok_collision_world.h"
#include "em1_preview_scene.h"
#include "em1_native_preview.h"
#include "em1_jigsaw_havok.h"
#include "em1_model_closure.h"
#include "em1_nif_container.h"
#include "em1_nif_controllers.h"
#include "em1_portable_pixels.h"
#include "em1_nif_roots.h"
#include "em1_level_source.h"
#include "em1_scene_files.h"
#include "em1_keyframe.h"
#include "em1_native_animations.h"
#include "em1_behavior_probe.h"
#include "em1_vfs.h"
#include "online_runtime.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <set>

PPC_EXTERN_FUNC(__imp__sub_82ECC850); // native in-place Havok pack loader
PPC_EXTERN_FUNC(__imp__sub_82559A08); // NiStream constructor, caller-owned outer object
PPC_EXTERN_FUNC(__imp__sub_82555510); // synchronous Load(buffer,length)
PPC_EXTERN_FUNC(__imp__sub_82559C00); // complete destructor, does not free outer object
PPC_EXTERN_FUNC(__imp__sub_82584A70); // NiSourceTexture::GetWidth
PPC_EXTERN_FUNC(__imp__sub_82584AC0); // NiSourceTexture::GetHeight
PPC_EXTERN_FUNC(__imp__sub_82808930); // NiActorManager original sequence-ID lookup, borrowed result
PPC_EXTERN_FUNC(__imp__sub_8238C448); // native removeReference / deleting destructor

REXCVAR_DEFINE_BOOL(em1_validate_particle_assets,false,"Debug","Probe original EM1 steam resources through native loaders without actor admission");
REXCVAR_DEFINE_BOOL(em1_validate_gear_assets,false,"Debug","Validate original gear visual and convex shape in native loaders");
REXCVAR_DEFINE_BOOL(em1_validate_static_physics_resource,false,"Debug","Private complete original static physics resource load, clone, query and release proof; no world admission");
REXCVAR_DEFINE_BOOL(em1_validate_behavior_resources,false,"Debug","Private original Havok behavior conversion and native load/readback/release proof; no NPC admission");
REXCVAR_DEFINE_BOOL(em1_validate_model,false,"Debug",
    "Developer-only original EM1 grass model native loader fixture; no scene attachment");
REXCVAR_DEFINE_BOOL(em1_validate_legacy_model,false,"Debug","Private original legacy NIF native loader/release check; source bytes unchanged and no scene attachment");
REXCVAR_DECLARE(bool,em2_private_profile);

REXCVAR_DEFINE_BOOL(em1_validate_static_models,false,"Debug","Load original courtyard architecture through native NiStream without scene attachment");
REXCVAR_DEFINE_STRING(em1_validate_level_models,"","Debug","Validate original entry-level NIF resources through native loading/publication/release; no world activation");
REXCVAR_DEFINE_BOOL(em1_validate_jigsaw_rays,false,"Debug","Compare fixed original MOPP rays with complete native collection and applicable source triangle math; no world admission");
REXCVAR_DEFINE_STRING(em1_validate_level_keyframes,"","Debug","Inspect all original KF roots from entry-level/global archives through native NiStream; no actor binding");
REXCVAR_DEFINE_STRING(em1_validate_level_animation_managers,"","Debug","Load complete original KFM/NIF/KF closures in native factories; no world activation");
REXCVAR_DEFINE_BOOL(em1_validate_animation_playback,false,"Debug","Execute detached original animation graphs through stock sequence selection/update");

namespace em2::em1 {
namespace {
std::atomic_bool attempted{false};
constexpr uint32_t kStack=32768,kObject=2048;
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
    if(next<=at)return false;
    at=std::min(end,next);
  }
  return true;
}
struct Restore {
  PPCContext& ctx;PPCContext saved;unsigned csr;DWORD last_error;
  Restore(PPCContext& context,DWORD error):ctx(context),saved(context),csr(_mm_getcsr()),last_error(error){}
  ~Restore(){ctx=saved;_mm_setcsr(csr);SetLastError(last_error);}
};
struct Scratch {
  rex::memory::Memory& memory;uint32_t address;
  Scratch(rex::memory::Memory& allocator,uint32_t bytes):memory(allocator),address(allocator.SystemHeapAlloc(bytes,32)){}
  ~Scratch(){if(address)memory.SystemHeapFree(address);}
};
uint32_t NativeChildPath(uint8_t* base,uint32_t first,std::span<const uint32_t> path,uint32_t node_table){
  uint32_t current=first;
  for(const auto index:path){
    if(!Mapped(base,current,0xfc) || PPC_LOAD_U32(current)!=node_table)return 0;
    const auto children=PPC_LOAD_U32(current+0xf4);const auto count=PPC_LOAD_U16(current+0xfa);
    if(index>=count || !Mapped(base,children,size_t(count)*4))return 0;
    current=PPC_LOAD_U32(children+index*4);
  }
  return current;
}
bool CallbacksAudited(uint8_t* base) {
  // Original NiStream callback registry. Never clear/replace live callbacks to
  // make a fixture pass; report unfamiliar registrations for a separate audit.
  constexpr uint32_t registry=0x833cdea0,final_callback=0x833cde9c;
  if(!Mapped(base,final_callback,8))return false;
  const auto last=PPC_LOAD_U32(final_callback),list=PPC_LOAD_U32(registry);
  if(last && last!=0x825fe328) {
    REXLOG_ERROR("[em1] native model fixture needs final callback audit: {:08X}",last);return false;
  }
  // 825FE328 only acts on NIF versions older than 20.3.0.10; ours is 20.6.5.0.
  if(!Mapped(base,list,16))return false;
  const auto count=PPC_LOAD_U16(list+10),active=PPC_LOAD_U16(list+12);
  if(count>64 || active>count)return false;
  if(!active)return true;
  const auto slots=PPC_LOAD_U32(list+4);
  if(!Mapped(base,slots,size_t(count)*4))return false;
  // Observed initialized retail chain. The first four are version-gated
  // legacy conversions (all older than our 20.6.5.0). The final three visit
  // only the freshly loaded roots through 829B4CC8: native mesh/material
  // metadata, shader parameter initialization, and flagged material setup.
  // Preserve their allocations/cache effects and let native failures surface;
  // these are required asset-load operations, not scene/world publication.
  static const std::set<uint32_t> original_callbacks{
      0x82600c18,0x830419a8,0x83041718,0x83040428,
      0x829bfe98,0x829c1f70,0x829c4b38,0x825fe328};
  bool allowed=true;
  for(uint32_t i=0;i<count;++i) {
    const auto callback=PPC_LOAD_U32(slots+i*4);
    if(callback && !original_callbacks.contains(callback)) {
      REXLOG_ERROR("[em1] native model fixture needs registered callback audit: {:08X}",callback);
      allowed=false;
    }
  }
  return allowed;
}
void Preflight(const ModelClosure& closure,bool particles=false) {
  nif::Check(closure.model.mesh_origins.size()<=1024 &&
      closure.texture_origins.size()<=512 && closure.model.bytes.size()<=64*1024*1024,
      "Original model exceeds native fixture shape/texture budget");
  nif::Nif model(closure.model.bytes);
  ValidateNifControllerBlocks(model);
  const auto root_plan=PlanContainedNifRoots(model);
  const auto root=root_plan.roots[0];
  nif::Check(root<model.blocks.size() && (model.types[model.blocks[root].type]=="NiNode" || model.types[model.blocks[root].type]=="JPSJigsawNode" ||
      model.types[model.blocks[root].type]=="NiSourceTexture" || model.types[model.blocks[root].type]=="NiCamera"),
      "Fixture root has no audited native node factory");
  const auto mesh_count=std::count_if(model.blocks.begin(),model.blocks.end(),[&](const auto& block){return model.types[block.type]=="NiMesh";});
  nif::Check(size_t(mesh_count)==closure.model.mesh_origins.size(),"NIF contains unprepared geometry");
  static const std::set<std::string> allowed{
    "NiNode","JPSJigsawNode","NiStringExtraData","NiIntegerExtraData","NiColorExtraData","NiMaterialProperty",
    "NiVertexColorProperty","NiShadeProperty","NiZBufferProperty","NiStencilProperty","NiMesh",
    "NiTexturingProperty","NiSourceTexture","NiSpecularProperty","NiAlphaProperty","NiPixelData",
    "NiBillboardNode","NiTransformController","NiTextureTransformController","NiTransformInterpolator",
    "NiTransformData","NiFloatInterpolator","NiFloatData","NiFloatExtraData","NiTextKeyExtraData","NiWireframeProperty",
    "NiVisController","NiBoolInterpolator","NiBoolData","NiCamera",
    "NiFloatExtraDataController","NiColorExtraDataController","NiColorInterpolator","NiColorData",
    "NiMaterialColorController","NiPoint3Interpolator","NiPosData","NiMorphWeightsController","NiMorphMeshModifier"};
  // Opt-in original-resource compatibility probe. These types occur in the two
  // authored steam NIFs and have matching class names in the retail image.
  // Native NiStream still resolves factories, validates payloads and links them;
  // allowing this probe is not approval to admit particles into a live world.
  static const std::set<std::string> particle_types{
    "NiPSParticleSystem","NiPSSimulator","NiPSSimulatorGeneralStep","NiPSSimulatorForcesStep",
    "NiPSSimulatorCollidersStep","NiPSSimulatorFinalStep","NiPSAlignedQuadGenerator",
    "NiPSBoxEmitter","NiPSCylinderEmitter","NiPSGravityFieldForce","NiPSEmitParticlesCtlr",
    "NiFloatInterpolator","NiFloatData","NiBoolInterpolator","NiBoolData",
    "NonUniformParticleSystem","NonUniformSimulatorGeneralStep","NonUniformQuadGenerator",
    "NiPSMeshEmitter","NiPSFacingQuadGenerator","NiPSDragFieldForce","NiPSTorusEmitter","NiPSBoundUpdater"};
  size_t systems=0;for(const auto& block:model.blocks)if(model.types[block.type]=="NiPSParticleSystem" || model.types[block.type]=="NonUniformParticleSystem")++systems;
  const bool particle_graph=particles || systems!=0;
  const bool morph_graph=std::find(model.types.begin(),model.types.end(),"NiMorphMeshModifier")!=model.types.end();
  if(particle_graph){
    nif::Check(systems>0 && systems<=8 && model.blocks.size()<=512,"Particle fixture exceeds audited asset bounds");
  }
  for(const auto& block:model.blocks) {
    const auto& type=model.types[block.type];
    nif::Check(allowed.contains(type) || (particle_graph && particle_types.contains(type)) || type.starts_with("NiDataStream\x01" "0\x01") ||
        type.starts_with("NiDataStream\x01" "1\x01") || (morph_graph && type.starts_with("NiDataStream\x01" "3\x01")),("Unaudited fixture block type: "+type).c_str());
    if(type=="NiColorExtraData"){
      // Native class is registered (image strings8204048C/8204094C). This
      // NIF version stores one name index followed by four float components.
      const auto data=block.Data();nif::Check(data.size()==20,"Invalid color extra-data record");
      for(unsigned at=4;at<20;at+=4)nif::Check(std::isfinite(std::bit_cast<float>(nif::U32(data,at,model.big))),"Nonfinite color extra-data");
    }
    if(type=="NiSourceTexture") {
      const auto data=block.Data();const uint64_t at=12ull+4ull*nif::U32(data,4,model.big);
      nif::Check(at+24==data.size() && data[size_t(at)]==0,
          "Fixture must not request external textures through EM2");
    }
  }
}
}
static bool ValidateLegacyModel(PPCContext& ctx,uint8_t* base,std::shared_ptr<RvzSource> source){
  Restore restore(ctx,GetLastError());std::string error;
  auto pak=PakSource::Open(source,"packfiles/sw_steama_shared.pak",error);std::vector<uint8_t> bytes;
  if(!pak || !pak->ReadOriginal("environments/_test/props/block.nif_wii",bytes,error)){
    REXLOG_ERROR("[em1] native legacy NIF source unavailable: {}",error);return false;
  }
  const auto limit=bytes.begin()+std::min<size_t>(128,bytes.size());
  const auto end=std::find(bytes.begin(),limit,'\n');
  const auto start=size_t(end-bytes.begin())+1;
  if(bytes.size()<64 || bytes.size()>65536 || end==limit || start+13>bytes.size() || nif::U32(bytes,start,false)!=0x14030109 ||
     bytes[start+4]!=0 || nif::U32(bytes,bytes.size()-8,true)!=1 || nif::U32(bytes,bytes.size()-4,true)!=0){
    REXLOG_ERROR("[em1] native legacy NIF fixture requires the original BE20.3.1.9 single-root file");return false;
  }
  auto validate=[&](const std::vector<uint8_t>& bytes,std::string_view label){
  Restore probe_restore(ctx,GetLastError());
  auto* kernel=rex::kernel::kernel_state();if(!kernel || !kernel->memory())return false;
  Scratch scratch(*kernel->memory(),kObject+bytes.size());
  if(!scratch.address || !Mapped(base,scratch.address,kObject+bytes.size(),true))return false;
  if(uint64_t(scratch.address)<ctx.r1.u32 && uint64_t(scratch.address)+kObject+bytes.size()>ctx.r1.u32-kStack)return false;
  const auto owner=scratch.address,input=owner+kObject;
  std::memset(base+owner,0,kObject+bytes.size());std::memcpy(base+input,bytes.data(),bytes.size());
  ctx.r1.u64-=2048;
  REXLOG_INFO("[em1] native legacy NIF begin: {} version20.3.1.9 bytes={}; no version rewrite",label,bytes.size());
  ctx.r3.u64=owner;__imp__sub_82559A08(ctx,base);
  ctx.r3.u64=owner;ctx.r4.u64=input;ctx.r5.u64=bytes.size();__imp__sub_82555510(ctx,base);
  const bool loaded=(ctx.r3.u32&255)!=0;
  const auto status=PPC_LOAD_U32(owner+796),roots=PPC_LOAD_U32(owner+360),slots=PPC_LOAD_U32(owner+352);
  bool retained=roots==1 && Mapped(base,slots,4) && Mapped(base,PPC_LOAD_U32(slots),16);
  if(retained){const auto object=PPC_LOAD_U32(slots);retained=PPC_LOAD_U32(object+4)>0 && Mapped(base,PPC_LOAD_U32(object),64);}
  const bool detached=PPC_LOAD_U32(owner+396)==0,unchanged=std::memcmp(base+input,bytes.data(),bytes.size())==0;
  const std::string message(reinterpret_cast<const char*>(base+owner+800),strnlen(reinterpret_cast<const char*>(base+owner+800),260));
  ctx.r3.u64=owner;__imp__sub_82559C00(ctx,base);const bool released=PPC_LOAD_U32(owner+360)==0;
  const bool passed=loaded && !status && retained && detached && unchanged && released;
  REXLOG_INFO("[em1] native legacy NIF result: source={} passed={} loaded={} status={} roots={} retained={} detached={} unchanged={} released={} message='{}'; no rendering proof",label,passed,loaded,status,roots,retained,detached,unchanged,released,message);
  return passed;
  };
  if(!validate(bytes,"original"))return false;
  ModelClosure closure;
  if(!ReadModelClosure(source,"packfiles/sw_steama_shared.pak","environments/_test/props/block.nif_wii",closure,error)){
    REXLOG_ERROR("[em1] native legacy source-qualified preparation failed: {}",error);return false;
  }
  const nif::Nif original(bytes),prepared(closure.model.bytes);
  bool preserved=prepared.version==original.version && prepared.blocks.size()==original.blocks.size() &&
      prepared.types==original.types && closure.missing_texture_origins.size()==1 && closure.texture_origins.empty();
  for(size_t i=0;i<original.blocks.size() && preserved;++i){
    const auto before=original.blocks[i].Data(),after=prepared.blocks[i].Data();
    preserved=before.size()==after.size();
    const auto type=original.types[original.blocks[i].type];
    const size_t name=type=="NiSourceTexture"?13+4ull*nif::U32(before,4,original.big):SIZE_MAX;
    for(size_t j=0;j<before.size() && preserved;++j)
      if(!(j>=name && j-name<4))preserved=before[j]==after[j];
  }
  if(!preserved){REXLOG_ERROR("[em1] legacy qualification changed original geometry/texture semantics");return false;}
  return validate(closure.model.bytes,"source-qualified-original-missing-image");
}
static bool ValidateModel(PPCContext& ctx,uint8_t* base,std::shared_ptr<RvzSource> source,const char* model,bool particles=false,std::string_view archive="packfiles/dbc_1st_zonec.pak",std::span<const std::string> level_archives={}) {
  Restore restore(ctx,GetLastError());
  auto* kernel=rex::kernel::kernel_state();
  ModelClosure closure;std::string error;
  if(!ReadModelClosure(source,archive,model,closure,error,nullptr,level_archives)) {
    REXLOG_ERROR("[em1] native model source rejected: {}",error);return false;
  }
  try {Preflight(closure,particles);} catch(const std::exception& exception) {
    REXLOG_ERROR("[em1] native model fixture preflight rejected: {}",exception.what());return false;
  }
  const nif::Nif prepared_nif(closure.model.bytes);const auto source_roots=PlanContainedNifRoots(prepared_nif);
  const auto bytes=uint32_t(kObject+closure.model.bytes.size());
  Scratch scratch(*kernel->memory(),bytes);
  if(!scratch.address || !Mapped(base,scratch.address,bytes,true))return false;
  if(uint64_t(scratch.address)<ctx.r1.u32 &&
      uint64_t(scratch.address)+bytes>ctx.r1.u32-kStack) {
    REXLOG_ERROR("[em1] native model scratch overlaps guest stack");return false;
  }
  const auto owner=scratch.address,input=owner+kObject;
  std::memset(base+owner,0,bytes);
  std::memcpy(base+input,closure.model.bytes.data(),closure.model.bytes.size());
  ctx.r1.u64-=2048; // Separate native ABI home slots from interrupted caller.
  REXLOG_INFO("[em1] native model load begin: {} ({} bytes)",model,closure.model.bytes.size());
  ctx.r3.u64=owner;__imp__sub_82559A08(ctx,base);
  // Unexpected guest exceptions propagate. Never guess how to destroy a
  // partially constructed native object. Ordinary load failures always release.
  ctx.r3.u64=owner;ctx.r4.u64=input;ctx.r5.u64=closure.model.bytes.size();
  __imp__sub_82555510(ctx,base);
  const bool loaded=(ctx.r3.u32&255)!=0;
  const auto status=PPC_LOAD_U32(owner+796),roots=PPC_LOAD_U32(owner+360);
  const auto slots=PPC_LOAD_U32(owner+352);
  bool retained=roots==source_roots.roots.size() && Mapped(base,slots,size_t(roots)*4);
  if(retained) {
    const auto object=PPC_LOAD_U32(slots);
    retained=Mapped(base,object,16) && PPC_LOAD_U32(object+4)>0 &&
        Mapped(base,PPC_LOAD_U32(object),64);
    const nif::Nif parsed(closure.model.bytes);
    const auto source_root=nif::U32(parsed.footer,4,parsed.big);
    if(retained && parsed.types[parsed.blocks[source_root].type]=="NiSourceTexture")
      REXLOG_INFO("[em1] native standalone texture object: source='{}' object={:08X} table={:08X}",model,object,PPC_LOAD_U32(object));
  }
  bool aliases=retained;
  std::vector<uint32_t> original_root_tables(source_roots.roots.size(),0);
  if(retained)for(size_t root=0;root<source_roots.roots.size();++root){const auto object=PPC_LOAD_U32(slots+uint32_t(root)*4);
    if(Mapped(base,object,16))original_root_tables[root]=PPC_LOAD_U32(object);}
  if(aliases && roots>1){
    const auto first=PPC_LOAD_U32(slots);const auto node_table=PPC_LOAD_U32(first);
    for(size_t root=1;root<source_roots.roots.size() && aliases;++root){
      const auto current=NativeChildPath(base,first,source_roots.child_paths[root],node_table);
      aliases=aliases && Mapped(base,current,16) && PPC_LOAD_U32(current+4)>0 &&
        Mapped(base,PPC_LOAD_U32(current),64) && current==PPC_LOAD_U32(slots+uint32_t(root)*4);
    }
    REXLOG_INFO("[em1] native original root aliases: source='{}' roots={} preserved={}; original footer unchanged",model,roots,aliases);
  }
  const bool detached=PPC_LOAD_U32(owner+396)==0;
  const bool unchanged=std::memcmp(base+input,closure.model.bytes.data(),closure.model.bytes.size())==0;
  const std::string diagnostic(reinterpret_cast<const char*>(base+owner+800),
      strnlen(reinterpret_cast<const char*>(base+owner+800),260));
  ctx.r3.u64=owner;__imp__sub_82559C00(ctx,base);
  const bool released=PPC_LOAD_U32(owner+360)==0;
  if(!loaded || status || !retained || !aliases || !detached || !unchanged || !released) {
    REXLOG_ERROR("[em1] native model fixture failed: loaded={} status={} roots={} retained={} detached={} unchanged={} released={} message='{}'",
        loaded,status,roots,retained,detached,unchanged,released,diagnostic);return false;
  }
  REXLOG_INFO("[em1] native NiStream model fixture passed: original='{}' roots={} meshes={} textures={} bytes={} source={}; objects released, no scene attachment/render proof",
      model,roots,closure.model.mesh_origins.size(),closure.model.textures,closure.model.bytes.size(),source->DiscId());
  {
    const nif::Nif parsed(closure.model.bytes);
    const auto root=nif::U32(parsed.footer,4,parsed.big);
    if(parsed.types[parsed.blocks[root].type]=="NiSourceTexture"){
      if(!Mapped(base,0x833da818,4))return false;
      const auto manager=PPC_LOAD_U32(0x833da818);
      if(!Mapped(base,manager,1804,true))return false;
      const auto before=PPC_LOAD_U32(manager+1464);NativeTextureResource texture;
      const bool published=PublishNativeTexture(ctx,base,manager,closure,texture,error);
      const auto object=texture.owner.root;std::string release_error;
      uint32_t width=0,height=0,expected_width=0,expected_height=0;
      bool dimensions=false;
      if(published){
        const auto source=parsed.blocks[root].Data();const auto at=12+4*nif::U32(source,4,parsed.big);
        const auto pixels=nif::U32(source,at+5,parsed.big);
        if(pixels<parsed.blocks.size() && parsed.types[parsed.blocks[pixels].type]=="NiPixelData" &&
            ValidatePortablePixels(parsed.blocks[pixels].Data(),parsed.big,error)){
          const auto data=parsed.blocks[pixels].Data();expected_width=nif::U32(data,71,parsed.big);expected_height=nif::U32(data,75,parsed.big);
          ctx.r3.u64=object;__imp__sub_82584A70(ctx,base);width=ctx.r3.u32;
          ctx.r3.u64=object;__imp__sub_82584AC0(ctx,base);height=ctx.r3.u32;
          dimensions=width==expected_width && height==expected_height;
        }
      }
      const bool released=ReleaseNativeTexture(ctx,base,texture,release_error);
      const bool restored=PPC_LOAD_U32(manager+1464)==before;
      REXLOG_INFO("[em1] native texture publication: source='{}' published={} object={:08X} dimensions={} native={}x{} original={}x{} released={} records_restored={} error='{}' release_error='{}'; no UI/world attachment",model,published,object,dimensions,width,height,expected_width,expected_height,released,restored,error,release_error);
      return published && dimensions && released && restored;
    }
  }
  // The original typed-resource caller at 8290793C loads this manager global.
  // Exercise the same factory and matching release before scene attachment.
  if(REXCVAR_GET(em1_validate_static_models) || particles || !REXCVAR_GET(em1_validate_level_models).empty()) {
    if(!Mapped(base,0x833da818,4))return false;
    const auto manager=PPC_LOAD_U32(0x833da818);
    if(!Mapped(base,manager,1804,true))return false;
    const auto before=PPC_LOAD_U32(manager+1464);
    if(REXCVAR_GET(em1_validate_jigsaw_rays) && closure.model.converted_jigsaw_aggregates){
      auto pak=PakSource::Open(source,archive,error);std::vector<uint8_t> original;
      if(!pak || !pak->ReadOriginal(model,original,error)){REXLOG_ERROR("[em1] MOPP ray source rejected: {}",error);return false;}
      nif::Nif original_model(original);std::vector<JigsawAggregateInfo> geometry;std::vector<nif::Bytes> packs;
      if(!AdaptJigsawHavokBlocks(original_model,geometry,error,&packs)){REXLOG_ERROR("[em1] MOPP ray conversion rejected: {}",error);return false;}
      for(size_t i=0;i<geometry.size();++i){
        if(geometry[i].triangles.empty()){REXLOG_INFO("[em1] Fixed triangle-ray samples do not cover convex-only '{}' aggregate={}",model,i);continue;}
        NativeHavokResource resource;NativeCollisionProof proof;Scratch ray_scratch(*kernel->memory(),256);
        const bool loaded=PublishNativeHavok(ctx,base,manager,source,closure.model.origin_key+":mopp-ray",packs[i],resource,error);
        const bool queried=loaded && ray_scratch.address && ProbeNativeJigsawRays(ctx,base,resource.owner.root,ray_scratch.address,geometry[i],proof,error);
        std::string release_error;const bool released=ReleaseNativeHavok(ctx,base,resource,release_error);
        REXLOG_INFO("[em1] original MOPP ray diagnostic: model='{}' loaded={} queried={} released={} hits={} misses={} fraction_error={} oracle='{}' error='{}' release_error='{}'; no world admission",model,loaded,queried,released,proof.hit_rays,proof.miss_rays,proof.largest_fraction_error,geometry[i].convex_parts?"native complete collection":"CPU source triangles + native collection",error,release_error);
        if(!loaded || !queried || !released)return false;
      }
    }
    NativeModelResource resource;
    REXLOG_INFO("[em1] native resource publication begin: '{}' manager={:08X}",model,manager);
    if(!PublishNativeModel(ctx,base,manager,closure,resource,error)) {
      REXLOG_ERROR("[em1] native resource publication refused '{}': {}",model,error);return false;
    }
    REXLOG_INFO("[em1] native resource retained '{}' root={:08X} handle={:08X}",model,resource.root,resource.handle);
    if(std::string_view(model).ends_with("/DBC_scaletest2.nif_wii")) {
      ArchitectureScene architecture;
      if(ReadCourtyardArchitectureScene(source,resource.native_name,architecture,error))
        REXLOG_INFO("[em1] authored architecture projection ready: definitions={} placements={} inherited={} output_entities={} output_components={} bytes={}; not yet activated",
            architecture.original_definitions,architecture.original_placements,architecture.resolved_ancestors,
            architecture.scene.entity_count,architecture.scene.component_count,architecture.scene.bytes.size());
      else REXLOG_ERROR("[em1] authored architecture projection refused: {}",error);
    }
    bool cached_aliases=true;
    if(source_roots.roots.size()>1){
      for(size_t root=0;root<source_roots.roots.size();++root){
        const auto object=NativeChildPath(base,resource.root,source_roots.child_paths[root],original_root_tables[0]);
        cached_aliases=cached_aliases && Mapped(base,object,16) && PPC_LOAD_U32(object)==original_root_tables[root] && PPC_LOAD_U32(object+4)>0;
      }
      REXLOG_INFO("[em1] native cached root graph: source='{}' original_roots={} descendants_preserved={}; no synthetic parent or dropped root",model,source_roots.roots.size(),cached_aliases);
    }
    bool particle_constructed=true;
    if(particles){
      ArchitectureScene particle_scene;
      const uint32_t actor=std::string_view(model).ends_with("common_steam_02.nif_wii")?98677:98431;
      particle_constructed=ReadCourtyardArchitectureScene(source,resource.native_name,particle_scene,error,{},{},1,actor);
      NativeArchitectureEntity entity;
      if(particle_constructed){
        particle_scene.scene=std::move(particle_scene.particle_prototype);
        particle_constructed=CreateNativeArchitectureEntity(ctx,base,particle_scene,resource,entity,error,true,NativePrototype::Particle);
      }
      REXLOG_INFO("[em1] native particle construction actor={} success={} entity={:08X} error='{}'; not admitted",actor,particle_constructed,entity.entity,error);
      bool complete=false;std::string cleanup_error;
      if(!DestroyNativeArchitectureEntity(ctx,base,entity,complete,cleanup_error) || !complete){
        REXLOG_ERROR("[em1] particle construction cleanup incomplete: {}",cleanup_error);
        // Retain resources if the native entity could still reference them.
        return false;
      }
    }
    if(!ReleaseNativeModel(ctx,base,resource,error) || PPC_LOAD_U32(manager+1464)!=before) {
      REXLOG_ERROR("[em1] native resource release failed '{}': {}; record count {} -> {}",
          model,error,before,PPC_LOAD_U32(manager+1464));return false;
    }
    REXLOG_INFO("[em1] native resource publish/release passed '{}'; manager record count restored",model);
    if(!particle_constructed || !cached_aliases)return false;
  }
  return true;
}
static bool ValidateKeyframe(PPCContext& ctx,uint8_t* base,std::shared_ptr<RvzSource> source,const std::string& archive,const std::string& path){
  Restore restore(ctx,GetLastError());KeyframeResource resource;std::string error;
  if(!ReadKeyframe(source,archive,path,resource,error)){REXLOG_ERROR("[em1] keyframe source rejected '{}': {}",path,error);return false;}
  auto* kernel=rex::kernel::kernel_state();const auto bytes=uint32_t(kObject+resource.bytes.size());Scratch scratch(*kernel->memory(),bytes);
  if(!scratch.address || !Mapped(base,scratch.address,bytes,true))return false;
  if(uint64_t(scratch.address)<ctx.r1.u32 && uint64_t(scratch.address)+bytes>ctx.r1.u32-kStack)return false;
  const auto owner=scratch.address,input=owner+kObject;std::memset(base+owner,0,bytes);std::memcpy(base+input,resource.bytes.data(),resource.bytes.size());ctx.r1.u64-=2048;
  REXLOG_INFO("[em1] native keyframe begin: archive='{}' resource='{}' original_sequences={}",archive,path,resource.document.sequences.size());
  ctx.r3.u64=owner;__imp__sub_82559A08(ctx,base);ctx.r3.u64=owner;ctx.r4.u64=input;ctx.r5.u64=resource.bytes.size();__imp__sub_82555510(ctx,base);
  const bool loaded=(ctx.r3.u32&255)!=0;const auto status=PPC_LOAD_U32(owner+796),roots=PPC_LOAD_U32(owner+360),slots=PPC_LOAD_U32(owner+352);
  const nif::Nif source_nif(resource.bytes);
  bool typed=roots==resource.document.sequences.size() && Mapped(base,slots,size_t(roots)*4),fields=typed;
  for(uint32_t i=0;i<roots && typed;++i){
    const auto object=PPC_LOAD_U32(slots+i*4);typed=Mapped(base,object,16) && PPC_LOAD_U32(object+4)>0;
    const auto table=typed?PPC_LOAD_U32(object):0;typed=typed && Mapped(base,table,16);
    // Original NiSequenceData table: +8 is NiRTTI; +12 is the distinct
    // numeric RTTI descriptor (its first word is an ID, not a name pointer).
    if(typed){const auto fn=PPC_LOAD_U32(table+8);typed=table==0x821d4c28 && fn==0x83013bd8;
      if(typed){ctx.r3.u64=object;PPC_CALL_INDIRECT_FUNC(fn);const auto rtti=ctx.r3.u32,name=Mapped(base,rtti,4)?PPC_LOAD_U32(rtti):0;
        typed=rtti==0x833eead0 && name==0x821d4c0c && Mapped(base,name,15) && std::memcmp(base+name,"NiSequenceData",15)==0;}}
    const auto original_root=nif::U32(source_nif.footer,4+i*4,source_nif.big);
    const auto sequence=std::find_if(resource.document.sequences.begin(),resource.document.sequences.end(),[&](const auto& s){return s.root==original_root;});
    if(!typed || sequence==resource.document.sequences.end() || !Mapped(base,object,0x48)){fields=false;continue;}
    const auto name=PPC_LOAD_U32(object+0xc);
    fields=fields && PPC_LOAD_U32(object+0x14)==sequence->evaluators && PPC_LOAD_U32(object+0x20)==std::bit_cast<uint32_t>(sequence->duration) &&
        PPC_LOAD_U32(object+0x28)==std::bit_cast<uint32_t>(sequence->frequency) && Mapped(base,name,sequence->name.size()+1) &&
        std::memcmp(base+name,sequence->name.c_str(),sequence->name.size()+1)==0;
  }
  const bool unchanged=std::memcmp(base+input,resource.bytes.data(),resource.bytes.size())==0;const bool detached=PPC_LOAD_U32(owner+396)==0;
  ctx.r3.u64=owner;__imp__sub_82559C00(ctx,base);const bool released=PPC_LOAD_U32(owner+360)==0;
  const bool passed=loaded && !status && typed && fields && unchanged && detached && released;
  REXLOG_INFO("[em1] native keyframe result: resource='{}' passed={} loaded={} status={} roots={} typed={} fields={} unchanged={} detached={} released={}; original curves retained, no actor playback proof",path,passed,loaded,status,roots,typed,fields,unchanged,detached,released);
  if(!passed)return false;
  const auto manager=PPC_LOAD_U32(0x833da818);if(!Mapped(base,manager,1468))return false;const auto records=PPC_LOAD_U32(manager+1464);
  NativeKeyframeResource published;const bool publication=PublishNativeKeyframe(ctx,base,manager,source,resource,published,error);
  const auto cached=published.sequences.size();bool cache_fields=publication && cached==resource.document.sequences.size();
  for(size_t i=0;i<cached && cache_fields;++i){const auto object=published.sequences[i];const auto original_root=nif::U32(source_nif.footer,4+i*4,source_nif.big);
    const auto sequence=std::find_if(resource.document.sequences.begin(),resource.document.sequences.end(),[&](const auto& s){return s.root==original_root;});
    cache_fields=sequence!=resource.document.sequences.end() && Mapped(base,object,0x48);
    if(cache_fields){const auto name=PPC_LOAD_U32(object+0xc);cache_fields=PPC_LOAD_U32(object+0x14)==sequence->evaluators &&
        PPC_LOAD_U32(object+0x20)==std::bit_cast<uint32_t>(sequence->duration) && PPC_LOAD_U32(object+0x28)==std::bit_cast<uint32_t>(sequence->frequency) &&
        Mapped(base,name,sequence->name.size()+1) && std::memcmp(base+name,sequence->name.c_str(),sequence->name.size()+1)==0;}}
  bool collision_rejected=false;
  if(publication){NativeKeyframeResource duplicate;std::string collision_error;const auto held=PPC_LOAD_U32(manager+1464);
    const bool duplicate_published=PublishNativeKeyframe(ctx,base,manager,source,resource,duplicate,collision_error,published.owner.native_name);
    collision_rejected=!duplicate_published && !duplicate.owner.record && collision_error=="Native keyframe name already exists" &&
        PPC_LOAD_U32(manager+1464)==held && PPC_LOAD_U32(published.owner.record+12)==1;
    if(duplicate.owner.record){std::string ignored;ReleaseNativeKeyframe(ctx,base,duplicate,ignored);}}
  bool shared_lifetime=false;
  if(publication){NativeKeyframeResource retained;std::string retain_error;const bool retained_ok=RetainNativeKeyframe(ctx,base,published,retained,retain_error);
    const auto array=published.array;const auto record=published.owner.record;
    const bool two=retained_ok && retained.array==array && retained.owner.record==record && PPC_LOAD_U32(record+12)==2;
    const bool released_one=retained_ok && ReleaseNativeKeyframe(ctx,base,retained,retain_error);
    shared_lifetime=two && released_one && PPC_LOAD_U32(record+12)==1 && Mapped(base,array,16) && PPC_LOAD_U16(array+10)==cached;
    if(retained.owner.record){std::string ignored;ReleaseNativeKeyframe(ctx,base,retained,ignored);}}
  std::string release_error;const bool cleanup=ReleaseNativeKeyframe(ctx,base,published,release_error);
  const bool restored=PPC_LOAD_U32(manager+1464)==records;
  REXLOG_INFO("[em1] native keyframe publication: archive='{}' resource='{}' published={} sequences={} cache_fields={} collision_rejected={} shared_lifetime={} released={} records_restored={} error='{}' release_error='{}'; no actor playback",archive,path,publication,cached,cache_fields,collision_rejected,shared_lifetime,cleanup,restored,error,release_error);
  return publication && cache_fields && collision_rejected && shared_lifetime && cleanup && restored;
}
static void ValidateBehaviorResources(PPCContext& ctx,uint8_t* base,std::shared_ptr<RvzSource> source){
  static BehaviorProbeResult result;std::string error;bool passed=false;
  try {
    nif::Check(REXCVAR_GET(em2_private_profile),"Behavior proof requires private settings");
    nif::Check(result.retained.empty(),"Previous behavior proof still owns native resources");
    const auto manager=Mapped(base,0x833da818,4)?PPC_LOAD_U32(0x833da818):0;
    nif::Check(Mapped(base,manager,1468),"Behavior proof needs initialized native pack manager");
    auto pak=PakSource::Open(source,"packfiles/meanstreet_visit1_wrapper.pak",error);
    nif::Check(bool(pak),error.c_str());std::vector<BehaviorProbeInput> inputs;
    for(const std::string_view path:{"Characters/pedestrianMale/pedestrianMaleBehaviorProject.hkx_wii",
        "Characters/pedestrianMale/Behaviors/pedestrianMaleBehavior.hkx_wii",
        "Characters/pedestrianMale/Characters/pedestrianMale.hkx_wii"}){
      BehaviorProbeInput input;input.origin=pak->OriginKey()+":behavior-proof1:"+std::string(path);
      nif::Check(pak->ReadOriginal(path,input.original,error),error.c_str());inputs.push_back(std::move(input));
    }
    passed=ProbeNativeBehaviorResources(ctx,base,manager,source,inputs,result,error);
  }catch(const std::exception& e){error=e.what();}
  for(const auto& r:result.resources)
    REXLOG_INFO("[em1] native behavior resource proof: origin='{}' cpu={} converted={} loaded={} fields={} released={} objects={} migrations={} dependencies={} checked_fields={} error='{}'",
        r.origin,r.cpu_checked,r.converted,r.loaded,r.fields_match,r.released,r.objects,r.semantic_migrations,r.dependencies,r.verified_fields,r.error);
  REXLOG_INFO("[em1] native behavior proof completed: passed={} resources={} records_restored={} retained={} error='{}'; no NPC attachment or graph activation",
      passed,result.resources.size(),result.records_restored,result.retained.size(),error);
}
static void ValidateStaticPhysicsResource(PPCContext& ctx,uint8_t* base,std::shared_ptr<RvzSource> source){
  std::string error,release_error;static NativeHavokResource resource;static uint32_t clone=0;
  bool converted=false,loaded=false,packed_matches=false,cloned_matches=false,released=false,records_restored=false;
  NativeCollisionProof packed_rays,clone_rays;
  auto* kernel=rex::kernel::kernel_state();
  const auto saved=ctx;auto reset=[&]{ctx=saved;ctx.r1.u64=uint64_t(saved.r1.u32)-2048;};
  const uint32_t manager=Mapped(base,0x833da818,4)?PPC_LOAD_U32(0x833da818):0;
  const auto records=Mapped(base,manager,1468)?PPC_LOAD_U32(manager+1464):0;
  try{
    nif::Check(REXCVAR_GET(em2_private_profile) && kernel && kernel->memory() && Mapped(base,manager,1468),"Static physics fixture needs private native manager");
    nif::Check(!resource.owner.record && !clone,"Previous static physics fixture still owns native references");
    auto pak=PakSource::Open(source,"packfiles/meanstreet_visit1_wrapper.pak",error);nif::Check(bool(pak),error.c_str());
    std::vector<uint8_t> original;
    nif::Check(pak->ReadOriginal("Environments/ThePark/MeanStreetUSA/MSUSA_Minihub_static_mesh.hkx_wii",original,error),error.c_str());
    AuthoredStaticCollision authored;ConvertedConvexBodyResource complete;
    nif::Check(ReconstructAuthoredStaticCollision(original,authored,error),error.c_str());
    nif::Check(TestStaticTriangleBodyResourceConversion(original,error),error.c_str());
    nif::Check(ConvertStaticTriangleBodyResource(original,complete,error),error.c_str());converted=true;
    loaded=PublishNativeHavok(ctx,base,manager,source,pak->OriginKey()+":complete-static-physics1",complete.pack,resource,error);
    nif::Check(loaded,error.c_str());
    auto text_matches=[&](uint32_t at,std::string_view text){return Mapped(base,at,text.size()+1) && !std::memcmp(base+at,text.data(),text.size()) && base[at+text.size()]==0;};
    const auto root=resource.owner.root;nif::Check(Mapped(base,root,12),"Native static root unavailable");
    const auto variants=PPC_LOAD_U32(root),variant_count=PPC_LOAD_U32(root+4);
    nif::Check(variant_count && variant_count<=64 && Mapped(base,variants,size_t(variant_count)*12),"Invalid native static variants");
    uint32_t physics=0;for(uint32_t i=0;i<variant_count;++i){const auto row=variants+i*12;
      if(text_matches(PPC_LOAD_U32(row+4),"hkpPhysicsData")){nif::Check(!physics,"Duplicate native physics data variant");physics=PPC_LOAD_U32(row+8);}}
    nif::Check(Mapped(base,physics,24) && PPC_LOAD_U32(physics+16)==1,"Native static resource requires one authored physics system");
    const auto systems=PPC_LOAD_U32(physics+12);nif::Check(Mapped(base,systems,4),"Native static system array unavailable");
    const auto system=PPC_LOAD_U32(systems);
    Scratch scratch(*kernel->memory(),256);nif::Check(scratch.address && Mapped(base,scratch.address,256,true),"Native static query scratch unavailable");
    auto inspect=[&](uint32_t object,NativeCollisionProof& rays){
      if(!Mapped(base,object,68) || PPC_LOAD_U32(object)!=0x8202e78c || PPC_LOAD_U32(object+12)!=authored.bodies.size())return false;
      const auto bodies=PPC_LOAD_U32(object+8);if(!Mapped(base,bodies,authored.bodies.size()*4))return false;
      std::vector<uint32_t> shapes;
      for(size_t i=0;i<authored.bodies.size();++i){const auto body=PPC_LOAD_U32(bodies+uint32_t(i)*4);const auto& expected=authored.bodies[i];
        if(!Mapped(base,body,544) || PPC_LOAD_U32(body)!=0x82030384 || PPC_LOAD_U32(body+8) ||
            PPC_LOAD_U32(body+0xc)!=expected.user_data || PPC_LOAD_U32(body+0x2c)!=expected.filter ||
            // hkStringPtr owns cloned names with bit0, unlike the borrowed
            // plain C strings in root named variants. 8238CD48 strips this
            // bit before comparison and sets it on each allocated clone.
            !text_matches(PPC_LOAD_U32(body+0x78)&~1u,expected.name) || PPC_LOAD_U32(body+0x80)!=expected.properties.size()){
          error="Native static body identity/name/filter/property mismatch at index "+std::to_string(i);return false;
        }
        for(unsigned c=0;c<4;++c)for(unsigned r=0;r<3;++r)
          if(PPC_LOAD_U32(body+0xf0+(c*4+r)*4)!=std::bit_cast<uint32_t>(expected.transform[c*4+r]))return false;
        if(PPC_LOAD_U32(body+0x8c)!=std::bit_cast<uint32_t>(expected.friction) || PPC_LOAD_U32(body+0x90)!=std::bit_cast<uint32_t>(expected.restitution))return false;
        if(!expected.properties.empty()){const auto properties=PPC_LOAD_U32(body+0x7c);
          if(!Mapped(base,properties,expected.properties.size()*16))return false;
          for(size_t j=0;j<expected.properties.size();++j)for(unsigned k=0;k<4;++k)
            if(PPC_LOAD_U32(properties+uint32_t(j)*16+k*4)!=expected.properties[j][k])return false;}
        shapes.push_back(PPC_LOAD_U32(body+0x10));
      }
      return ProbeNativeLoadedCollisionShapes(ctx,base,scratch.address,shapes,authored,rays,error);
    };
    packed_matches=inspect(system,packed_rays);nif::Check(packed_matches,error.empty()?"Native static resource body graph or queries differ from authored data":error.c_str());
    const auto table=PPC_LOAD_U32(system);nif::Check(Mapped(base,table,12),"Native system clone interface unavailable");
    const auto method=PPC_LOAD_U32(table+8);nif::Check(method>=0x82200000 && method<0x83340000 && !(method&3) && Mapped(base,method,4),"Native system clone method invalid");
    reset();ctx.r3.u64=system;PPC_CALL_INDIRECT_FUNC(method);clone=ctx.r3.u32;
    nif::Check(clone && clone!=system && Mapped(base,clone,68) && PPC_LOAD_U16(clone+6)==1,"Native physics system clone failed");
    cloned_matches=inspect(clone,clone_rays);nif::Check(cloned_matches,error.empty()?"Cloned static body graph or queries differ from authored data":error.c_str());
  }catch(const std::exception& failure){error=failure.what();}
  // Same ownership order as the stock component: release its cloned system
  // before the HKX resource backing its shared shapes/names/property arrays.
  if(clone && Mapped(base,clone,68) && PPC_LOAD_U32(clone)==0x8202e78c && PPC_LOAD_U16(clone+6)==1){
    reset();ctx.r3.u64=clone;__imp__sub_8238C448(ctx,base);clone=0;
  }
  if(!clone)released=ReleaseNativeHavok(ctx,base,resource,release_error);
  if(Mapped(base,manager,1468))records_restored=PPC_LOAD_U32(manager+1464)==records;
  REXLOG_INFO("[em1] complete static physics resource proof: converted={} loaded={} packed={} cloned={} packed_hits={} packed_misses={} clone_hits={} clone_misses={} released={} records_restored={} error='{}' release_error='{}'; no world admission",
      converted,loaded,packed_matches,cloned_matches,packed_rays.hit_rays,packed_rays.miss_rays,clone_rays.hit_rays,clone_rays.miss_rays,released,records_restored,error,release_error);
}
void MaybeValidateNativeModel(PPCContext& ctx,uint8_t* base,bool world_tick) {
  // Particle entity construction requires an idle world, not scene-begin.
  if(world_tick!=REXCVAR_GET(em1_validate_particle_assets))return;
  const auto incoming=GetLastError();
  const auto level_models=REXCVAR_GET(em1_validate_level_models);
  const auto level_keyframes=REXCVAR_GET(em1_validate_level_keyframes);
  const auto level_animation_managers=REXCVAR_GET(em1_validate_level_animation_managers);
  if((level_animation_managers.empty() && level_keyframes.empty() && level_models.empty() && !REXCVAR_GET(em1_validate_legacy_model) && !REXCVAR_GET(em1_validate_model) && !REXCVAR_GET(em1_validate_static_models) && !REXCVAR_GET(em1_validate_gear_assets) && !REXCVAR_GET(em1_validate_static_physics_resource) && !REXCVAR_GET(em1_validate_behavior_resources) && !REXCVAR_GET(em1_validate_particle_assets)) || attempted.exchange(true)){SetLastError(incoming);return;}
  Restore restore(ctx,incoming);
  if(em2::net::SynchronizedSessionActive()) {
    REXLOG_ERROR("[em1] native model fixture refused during synchronized online play");return;
  }
  auto* kernel=rex::kernel::kernel_state();
  if(!kernel || !kernel->memory() || !kernel->file_system())return;
  auto source=MountedRvz(*kernel->file_system());
  if(!source){REXLOG_ERROR("[em1] native model fixture requires em1_source_rvz");return;}
  if(REXCVAR_GET(em1_validate_behavior_resources)){
    if(ctx.r1.u32<kStack || !Mapped(base,ctx.r1.u32-kStack,kStack,true))return;
    ValidateBehaviorResources(ctx,base,source);return;
  }
  if(REXCVAR_GET(em1_validate_static_physics_resource)){
    if(ctx.r1.u32<kStack || !Mapped(base,ctx.r1.u32-kStack,kStack,true))return;
    ValidateStaticPhysicsResource(ctx,base,source);return;
  }
  if(REXCVAR_GET(em1_validate_legacy_model)){
    if(!REXCVAR_GET(em2_private_profile) || ctx.r1.u32<kStack || !Mapped(base,ctx.r1.u32-kStack,kStack,true) || !CallbacksAudited(base))return;
    ValidateLegacyModel(ctx,base,source);return;
  }
  if(!level_animation_managers.empty()){
    if(ctx.r1.u32<kStack || !Mapped(base,ctx.r1.u32-kStack,kStack,true) || !CallbacksAudited(base))return;
    LevelContents contents;std::string error;if(!ReadLevelContents(source,level_animation_managers,contents,error)){REXLOG_ERROR("[em1] animation inventory rejected: {}",error);return;}
    std::set<std::string> archives{"packfiles/_dynamic.pak"};for(const auto& scene:contents.entry_scenes)archives.insert(scene.request.archive);
    std::vector<std::string> scoped(archives.begin(),archives.end());size_t passed=0,total=0;
    const auto manager=PPC_LOAD_U32(0x833da818);if(!Mapped(base,manager,1468))return;
    for(const auto& archive:archives){auto pak=PakSource::Open(source,archive,error);if(!pak){REXLOG_ERROR("[em1] animation archive unavailable: {}",error);return;}
      for(const auto& entry:pak->Entries())if(entry.type==std::array<uint8_t,4>{'K','F','M',0}){
        if(++total>256){REXLOG_ERROR("[em1] animation manager diagnostic exceeds budget");return;}
        KfmClosure closure;if(!ReadKfmClosure(source,archive,entry.path,closure,error,scoped)){REXLOG_ERROR("[em1] animation closure rejected '{}': {}",entry.path,error);continue;}
        const auto before=PPC_LOAD_U32(manager+1464);NativeAnimationBundle bundle;
        REXLOG_INFO("[em1] native animation manager begin: archive='{}' resource='{}' models={} keyframe_files={} sequences={}",archive,entry.path,closure.models.size(),closure.keyframes.size(),closure.document.sequences);
        const bool loaded=PublishNativeAnimationBundle(ctx,base,manager,closure,bundle,error);
        const auto object=bundle.animation.root,table=Mapped(base,object,16)?PPC_LOAD_U32(object):0;
        const auto model_root=Mapped(base,object,0xac)?PPC_LOAD_U32(object+0x14):0,controller=Mapped(base,object,0xac)?PPC_LOAD_U32(object+0x18):0;
        bool bound=loaded && table==0x821d4bf8 && Mapped(base,model_root,16) && Mapped(base,controller,16);size_t sequences=0;
        if(bound)for(const auto id:closure.document.sequence_ids){ctx.r3.u64=object;ctx.r4.u64=id;__imp__sub_82808930(ctx,base);const auto sequence=ctx.r3.u32;
          if(!Mapped(base,sequence,0x48) || PPC_LOAD_U32(sequence)!=0x821d4c28){bound=false;break;}++sequences;}
        if(bound && REXCVAR_GET(em1_validate_animation_playback)){
          std::vector<uint32_t> nodes,pending{model_root};std::set<uint32_t> visited;const auto node_table=PPC_LOAD_U32(model_root);
          while(!pending.empty() && nodes.size()<4096){const auto node=pending.back();pending.pop_back();if(!visited.insert(node).second || !Mapped(base,node,208))continue;nodes.push_back(node);
            if(PPC_LOAD_U32(node)==node_table && Mapped(base,node,0xfc)){const auto children=PPC_LOAD_U32(node+0xf4);const auto count=PPC_LOAD_U16(node+0xfa);
              if(count<=4096 && (!count || Mapped(base,children,size_t(count)*4)))for(uint32_t i=0;i<count;++i){const auto child=PPC_LOAD_U32(children+i*4);if(child)pending.push_back(child);}}}
          bool finite=true;
          auto snapshot=[&](){std::vector<uint32_t> values;for(const auto node:nodes)for(const unsigned i:{0,1,2,4,5,6,8,9,10,12,13,14,15}){
              const auto value=PPC_LOAD_U32(node+144+i*4);finite=finite && std::isfinite(std::bit_cast<float>(value));values.push_back(value);}return values;};
          size_t selected=0,updated=0,changed=0;bool playback=true;float clock=0;
          for(const auto id:closure.document.sequence_ids){if(!SelectNativeAnimationSequence(ctx,base,bundle.animation,id,error)){playback=false;break;}++selected;
            if(!UpdateNativeAnimation(ctx,base,bundle.animation,clock,error)){playback=false;break;}auto previous=snapshot();
            for(unsigned step=0;step<8;++step){clock+=0.125f;if(!UpdateNativeAnimation(ctx,base,bundle.animation,clock,error)){playback=false;break;}++updated;auto current=snapshot();if(current!=previous)++changed;previous=std::move(current);}
            if(!playback)break;}
          REXLOG_INFO("[em1] native animation playback: resource='{}' selected={} updates={} observed_nodes={} changed_transform_samples={} finite={} success={} error='{}'; detached original graphs, no world/render proof",entry.path,selected,updated,nodes.size(),changed,finite,playback,error);
          bound=bound && playback && finite;
        }
        std::string cleanup_error;const bool released=ReleaseNativeAnimationBundle(ctx,base,bundle,cleanup_error);const bool restored=PPC_LOAD_U32(manager+1464)==before;
        REXLOG_INFO("[em1] native animation manager result: archive='{}' resource='{}' loaded={} object={:08X} table={:08X} bound={} model={:08X} controller={:08X} sequences={} released={} records_restored={} error='{}' cleanup_error='{}'; no world/scene attachment",archive,entry.path,loaded,object,table,bound,model_root,controller,sequences,released,restored,error,cleanup_error);
        if(loaded && bound && released && restored)++passed;
        if(!released){REXLOG_ERROR("[em1] animation closure cleanup incomplete; stopping");return;}
      }}
    REXLOG_INFO("[em1] original animation manager scope diagnostic finished: level='{}' accepted={}/{}; no world activation",level_animation_managers,passed,total);return;
  }
  if(!level_keyframes.empty()){
    if(ctx.r1.u32<kStack || !Mapped(base,ctx.r1.u32-kStack,kStack,true) || !CallbacksAudited(base))return;
    LevelContents contents;std::string error;if(!ReadLevelContents(source,level_keyframes,contents,error)){REXLOG_ERROR("[em1] keyframe inventory rejected: {}",error);return;}
    std::set<std::string> archives{"packfiles/_dynamic.pak"};for(const auto& scene:contents.entry_scenes)archives.insert(scene.request.archive);
    size_t passed=0,total=0;
    for(const auto& archive:archives){auto pak=PakSource::Open(source,archive,error);if(!pak){REXLOG_ERROR("[em1] keyframe archive failed: {}",error);return;}
      for(const auto& entry:pak->Entries())if(entry.type==std::array<uint8_t,4>{'K','F',0,0}){
        if(++total>1024){REXLOG_ERROR("[em1] keyframe diagnostic exceeds budget");return;}if(ValidateKeyframe(ctx,base,source,archive,entry.path))++passed;
      }}
    REXLOG_INFO("[em1] original keyframe scope diagnostic finished: level='{}' accepted={}/{}; no actor/world activation",level_keyframes,passed,total);return;
  }
  if(!level_models.empty()){
    if(ctx.r1.u32<kStack || !Mapped(base,ctx.r1.u32-kStack,kStack,true) || !CallbacksAudited(base))return;
    LevelContents contents;std::string error;
    if(!ReadLevelContents(source,level_models,contents,error)){REXLOG_ERROR("[em1] level model inventory rejected: {}",error);return;}
    std::set<std::pair<std::string,std::string>> models;
    std::vector<std::string> level_archives;
    for(const auto& scene:contents.entry_scenes){
      if(std::find(level_archives.begin(),level_archives.end(),scene.request.archive)==level_archives.end())
        level_archives.push_back(scene.request.archive);
      std::vector<SceneFileReference> references;if(!CollectSceneFileReferences(scene.resource.scene,references,error)){REXLOG_ERROR("[em1] level model references rejected: {}",error);return;}
      for(auto& reference:references){
        for(auto& ch:reference.path){if(ch=='\\')ch='/';if(ch>='A' && ch<='Z')ch+=32;}
        if(reference.path.ends_with(".nif"))reference.path+="_wii";
        else if(!reference.path.ends_with(".nif_wii"))continue;
        models.emplace(scene.request.archive,std::move(reference.path));
      }
    }
    if(models.size()>1024){REXLOG_ERROR("[em1] level model diagnostic exceeds asset budget");return;}
    unsigned passed=0;
    for(const auto& [archive,path]:models){
      if(ValidateModel(ctx,base,source,path.c_str(),false,archive,level_archives))++passed;
      else REXLOG_ERROR("[em1] level model unavailable: archive='{}' path='{}'",archive,path);
    }
    REXLOG_INFO("[em1] complete entry-level model diagnostic finished: level='{}' accepted={}/{}; no entity/component/world activation",level_models,passed,models.size());return;
  }
  if(REXCVAR_GET(em1_validate_particle_assets)) {
    if(ctx.r1.u32<kStack || !Mapped(base,ctx.r1.u32-kStack,kStack,true) || !CallbacksAudited(base))return;
    const bool blast=ValidateModel(ctx,base,source,"effects/_shared/common_steam_02.nif_wii",true);
    const bool jet=ValidateModel(ctx,base,source,"effects/_shared/steam_jet_01.nif_wii",true);
    REXLOG_INFO("[em1] native steam asset probe completed blast={} jet={}; no actor admission or emission proof",blast,jet);return;
  }
  if(REXCVAR_GET(em1_validate_gear_assets)) {
    if(ctx.r1.u32<kStack || !Mapped(base,ctx.r1.u32-kStack,kStack,true) || !CallbacksAudited(base))return;
    const bool visual=ValidateModel(ctx,base,source,"environments/thepark/darkbeautycastle/zones/dbc_zoneb_trebgeara1.nif_wii");
    std::string error;auto pak=PakSource::Open(source,"packfiles/dbc_1st_zonec.pak",error);
    std::vector<uint8_t> original;ConvertedConvexBodyShape converted;
    bool shape=false;
    if(pak && pak->ReadOriginal("environments/thepark/darkbeautycastle/zones/dbc_zoneb_trebgeara1_dynamic_hull.hkx_wii",original,error) && ConvertConvexBodyShape(original,converted,error)){
      try{
        Scratch scratch(*kernel->memory(),uint32_t(converted.pack.size()));
        nif::Check(scratch.address && Mapped(base,scratch.address,converted.pack.size(),true),"Gear hull scratch unavailable");
        nif::Check(Mapped(base,0x833CCF18,4) && PPC_LOAD_U32(0x833CCF18),"Native Havok registry unavailable");
        std::memcpy(base+scratch.address,converted.pack.data(),converted.pack.size());
        ctx.r1.u64-=2048;ctx.r3.u64=scratch.address;ctx.r4.u64=converted.pack.size();ctx.r5.u64=0;ctx.r6.u64=0;
        __imp__sub_82ECC850(ctx,base);const auto root=ctx.r3.u32;
        auto inside=[&](uint32_t at,uint64_t n){return at>=scratch.address && uint64_t(at)+n<=uint64_t(scratch.address)+converted.pack.size();};
        nif::Check(root && inside(root,0x70),"Native convex root outside pack");
        const auto vertices=PPC_LOAD_U32(root+0x4C),groups=PPC_LOAD_U32(root+0x44),data=PPC_LOAD_U32(root+0x40);
        const auto planes=PPC_LOAD_U32(root+0x54),plane_count=PPC_LOAD_U32(root+0x58);
        nif::Check(vertices==converted.vertices.size() && groups==(vertices+3)/4 && inside(data,uint64_t(groups)*48),"Native convex vertex array differs");
        nif::Check(plane_count==converted.planes.size() && inside(planes,uint64_t(plane_count)*16),"Native convex plane array differs");
        for(unsigned i=0;i<vertices;++i)for(unsigned j=0;j<3;++j)nif::Check(PPC_LOAD_U32(data+(i/4)*48+j*16+(i%4)*4)==std::bit_cast<uint32_t>(converted.vertices[i][j]),"Native convex vertex differs from original");
        for(unsigned i=0;i<plane_count;++i)for(unsigned j=0;j<4;++j)nif::Check(PPC_LOAD_U32(planes+i*16+j*4)==std::bit_cast<uint32_t>(converted.planes[i][j]),"Native convex plane differs from original");
        static NativeHavokResource published;
        const auto manager=Mapped(base,0x833DA818,4)?PPC_LOAD_U32(0x833DA818):0;
        const bool publication=PublishNativeHavok(ctx,base,manager,source,pak->OriginKey()+":gear-convex1",converted.pack,published,error);
        bool published_geometry=false;
        if(publication && Mapped(base,published.owner.root,0x70)){
          const auto live=published.owner.root,v=PPC_LOAD_U32(live+0x40),p=PPC_LOAD_U32(live+0x54);
          published_geometry=PPC_LOAD_U32(live+0x4C)==vertices && PPC_LOAD_U32(live+0x58)==plane_count && Mapped(base,v,uint64_t(groups)*48) && Mapped(base,p,uint64_t(plane_count)*16);
          if(published_geometry){
            for(unsigned i=0;i<vertices;++i)for(unsigned j=0;j<3;++j)published_geometry&=PPC_LOAD_U32(v+(i/4)*48+j*16+(i%4)*4)==std::bit_cast<uint32_t>(converted.vertices[i][j]);
            for(unsigned i=0;i<plane_count;++i)for(unsigned j=0;j<4;++j)published_geometry&=PPC_LOAD_U32(p+i*16+j*4)==std::bit_cast<uint32_t>(converted.planes[i][j]);
          }
        }
        std::string release_error;const bool released=ReleaseNativeHavok(ctx,base,published,release_error);
        REXLOG_INFO("[em1] native gear HKX resource publication={} geometry={} released={} error='{}' release_error='{}'",publication,published_geometry,released,error,release_error);
        nif::Check(publication && published_geometry && released,"Native gear HKX resource lifecycle failed");
        ConvertedConvexBodyResource full;
        nif::Check(ConvertConvexBodyResource(original,full,error),error.c_str());
        const auto data_section=(uint32_t(original.at(180))<<24)|(uint32_t(original.at(181))<<16)|(uint32_t(original.at(182))<<8)|original.at(183);
        for(unsigned fault=0;fault<2;++fault){
          auto invalid=original;const auto body=data_section+converted.source_body_offset;
          if(fault==0)invalid.at(body+0xe8)=4;
          else {invalid.at(body+0x1cc)=0x7f;invalid.at(body+0x1cd)=0xc0;invalid.at(body+0x1ce)=0;invalid.at(body+0x1cf)=0;}
          ConvertedConvexBodyResource rejected;std::string rejected_error;
          nif::Check(!ConvertConvexBodyResource(invalid,rejected,rejected_error) && rejected.pack.empty() && !rejected_error.empty(),"Unsafe body fixture accepted");
        }
        static NativeHavokResource body_resource;
        const bool body_loaded=PublishNativeHavok(ctx,base,manager,source,pak->OriginKey()+":gear-body1",full.pack,body_resource,error);
        auto check_body=[&](){
          if(!body_loaded)return false;
          const auto root=body_resource.owner.root,body=root+full.body_offset;
          if(!Mapped(base,root,12) || !Mapped(base,body,0x220))return false;
          const auto variants=PPC_LOAD_U32(root),count=PPC_LOAD_U32(root+4);
          if(count!=3 || !Mapped(base,variants,count*12))return false;
          // The authored third named variant is PhysicsData. Traverse its real
          // loaded pointers, rather than only inspecting an unreferenced body.
          const auto physics=PPC_LOAD_U32(variants+32);
          if(!Mapped(base,physics,24) || PPC_LOAD_U32(physics+16)!=1)return false;
          const auto systems=PPC_LOAD_U32(physics+12);
          if(!Mapped(base,systems,4))return false;
          const auto system=PPC_LOAD_U32(systems);
          if(!Mapped(base,system,0x44) || PPC_LOAD_U32(system+12)!=1)return false;
          const auto bodies=PPC_LOAD_U32(system+8);
          if(!Mapped(base,bodies,4) || PPC_LOAD_U32(bodies)!=body)return false;
          const auto hull=PPC_LOAD_U32(body+0x10);
          if(!Mapped(base,hull,0x70) || PPC_LOAD_U32(hull+0x4c)!=vertices)return false;
          auto word=[&](size_t at){return uint32_t(original.at(at))<<24|uint32_t(original.at(at+1))<<16|uint32_t(original.at(at+2))<<8|original.at(at+3);};
          const auto old=word(180)+converted.source_body_offset;
          // Transform/swept state and mass/inertia/velocities must survive the
          // native finishing constructors, not just the serialization step.
          for(unsigned off=0xf0;off<0x194;off+=4)if(PPC_LOAD_U32(body+off)!=word(old+off))return false;
          for(unsigned off=0x1a0;off<0x1d0;off+=4)if(PPC_LOAD_U32(body+off)!=word(old+off))return false;
          return PPC_LOAD_U8(body+0xda)==original.at(old+0xde) && PPC_LOAD_U8(body+0xdb)==original.at(old+0xdf) &&
            PPC_LOAD_U32(body+0x8c)==word(old+0x90) && PPC_LOAD_U32(body+0x90)==word(old+0x94) && PPC_LOAD_U16(body+0x8a)==0 &&
            PPC_LOAD_U8(body+0xe8)==converted.motion_type &&
            PPC_LOAD_U16(body+0x194)==uint16_t(word(old+0x194)>>16) &&
            PPC_LOAD_U16(body+0x196)==uint16_t(word(old+0x198)>>16) && PPC_LOAD_U16(body+0x198)==0x3f80 &&
            PPC_LOAD_U8(body+0x19a)==original.at(old+0x19c) && PPC_LOAD_U8(body+0x19b)==original.at(old+0x19d);
        };
        const bool body_matches=check_body();
        std::string body_release_error;const bool body_released=ReleaseNativeHavok(ctx,base,body_resource,body_release_error);
        REXLOG_INFO("[em1] native gear full body loaded={} authored_motion={} released={} bytes={} error='{}' release_error='{}'; no world admission",body_loaded,body_matches,body_released,full.pack.size(),error,body_release_error);
        nif::Check(body_loaded && body_matches && body_released,"Native full gear body validation failed");
        shape=true;
        REXLOG_INFO("[em1] native gear convex payload passed vertices={} planes={} motion_type={} source_body={} bytes={}; no dynamic body admission",vertices,plane_count,converted.motion_type,converted.source_body_offset,converted.pack.size());
      }catch(const std::exception& exception){error=exception.what();}
    }
    REXLOG_INFO("[em1] native gear assets completed visual={} convex={} error='{}'",visual,shape,error);return;
  }
  if(REXCVAR_GET(em1_validate_static_models)) {
    std::string error;
    auto pak=PakSource::Open(source,"packfiles/dbc_1st_zonec.pak",error);
    if(pak) {
      std::vector<uint8_t> original_jigsaw;
      if(pak->ReadOriginal("environments/thepark/darkbeautycastle/zones/dbc_zoneb_toon02.nif_wii",original_jigsaw,error)) {
        try {
          nif::Nif model(original_jigsaw);
          bool aggregate_ok=false;
          for(const auto& block:model.blocks)if(model.types[block.type]=="JPSJigsawNode") {
            const auto data=block.Data();
            nif::Check(data.size()>=390,"Truncated original Jigsaw aggregate fixture");
            const auto length=nif::U32(data,386,model.big);
            nif::Check(length<=data.size()-390,"Original Jigsaw aggregate exceeds block");
            aggregate_ok=TestJigsawAggregateConversion(data.subspan(390,length),error);
          }
          std::vector<JigsawAggregateInfo> proof;
          const bool adapted=aggregate_ok && AdaptJigsawHavokBlocks(model,proof,error);
          if(adapted && proof.size()==1)
            REXLOG_INFO("[em1] original Jigsaw CPU conversion passed: triangles={} convex_vertices={} triangle_parts={} convex_parts={} mopp_bytes={}; native loading/MOPP parity unproven",
                proof[0].triangles.size(),proof[0].convex_vertices.size(),proof[0].triangle_parts,proof[0].convex_parts,proof[0].mopp_bytes);
          else REXLOG_ERROR("[em1] original Jigsaw CPU conversion refused: {}",error);
        }catch(const std::exception& exception){REXLOG_ERROR("[em1] original Jigsaw CPU fixture rejected: {}",exception.what());}
      }else REXLOG_ERROR("[em1] original Jigsaw fixture source unavailable: {}",error);
    }
    std::string collision_path;
    unsigned matches=0;
    if(pak)for(const auto& entry:pak->Entries()) {
      std::string lower=entry.path;
      for(auto& c:lower)if(c>='A' && c<='Z')c+='a'-'A';
      if(lower.ends_with("/dbc_scaletest2_static_mesh.hkx_wii")){collision_path=entry.path;++matches;}
    }
    std::vector<uint8_t> original;
    AuthoredStaticCollision collision;
    const bool reconstructed=pak && matches==1 && pak->ReadOriginal(collision_path,original,error) &&
        ReconstructAuthoredStaticCollision(original,collision,error);
    if(reconstructed && collision.bodies.size()==3 && collision.vertices==10948 && collision.triangles==16565) {
      REXLOG_INFO("[em1] original collision reconstruction passed '{}': {} bodies, {} vertices, {} triangles; native loading/world admission not yet tested",
          collision_path,collision.bodies.size(),collision.vertices,collision.triangles);
      const auto size=NativeCollisionProbeScratchSize(collision);
      if(size && size<=64*1024*1024) {
        Scratch memory(*kernel->memory(),uint32_t(size));
        if(memory.address && Mapped(base,memory.address,size,true)) {
          std::memset(base+memory.address,0,size);
          NativeCollisionProof proof;
          const bool accepted=ProbeNativeAuthoredCollision(ctx,base,memory.address,size,collision,proof,error);
          REXLOG_INFO("[em1] native authored collision probe accepted={} loaded={} hits={} misses={} fraction_error={} error='{}'; no world admission",
              accepted,proof.loaded_shapes,proof.hit_rays,proof.miss_rays,proof.largest_fraction_error,error);
          if(accepted) {
            NativeCollisionAttachment attachment;
            const bool constructed=ConstructNativeAuthoredCollision(ctx,base,collision,kIdentityCollisionPlacement,attachment,error);
            const auto world=CurrentNativeCollisionWorld(base);
            const bool admitted=constructed && world && AdmitNativeAuthoredCollision(ctx,base,world,attachment,error);
            const bool floor=admitted && ProbeNativeCollisionFloorRay(ctx,base,attachment,error);
            const auto outcome=error;
            const bool released=ReleaseNativeAuthoredCollision(ctx,base,attachment,error);
            REXLOG_INFO("[em1] authored collision world fixture: constructed={} admitted={} floor={} released={} world={:08X} error='{}' cleanup='{}'; no player contact acceptance",
                constructed,admitted,floor,released,world,outcome,error);
          }
        }
      }
    }
    else REXLOG_ERROR("[em1] original collision reconstruction failed: matches={} bodies={} vertices={} triangles={} error='{}'",
          matches,collision.bodies.size(),collision.vertices,collision.triangles,error);
  }
  if(ctx.r1.u32<kStack || !Mapped(base,ctx.r1.u32-kStack,kStack,true)) {
    REXLOG_ERROR("[em1] native model fixture lacks 32 KiB writable guest stack");return;
  }
  if(!CallbacksAudited(base)) {
    REXLOG_ERROR("[em1] native model fixture stopped before allocation: callback registry not audited");return;
  }
  static constexpr const char* models[]={
    "Environments/ThePark/DarkBeautyCastle/Props/Misc/DBC_GrassA1_Toon.nif_wii",
    "environments/thepark/darkbeautycastle/Zones/DBC_scaletest2.nif_wii",
    "environments/thepark/darkbeautycastle/Zones/DBC_ZoneB_toon01.nif_wii",
    // ZoneB_toon02 Jigsaw payload requires embedded Havok adaptation before native loading.
    "environments/thepark/darkbeautycastle/Zones/Ramp_A_DBCMain.nif_wii",
    "environments/thepark/darkbeautycastle/Zones/DBC_toonrampI.nif_wii"};
  const unsigned count=REXCVAR_GET(em1_validate_static_models)?unsigned(std::size(models)):1;
  unsigned passed=0;
  for(unsigned i=0;i<count;++i) {
    if(ValidateModel(ctx,base,source,models[i]))++passed;
    else REXLOG_ERROR("[em1] native model validation refused original '{}'",models[i]);
  }
  REXLOG_INFO("[em1] native model validation completed: {}/{} accepted; no scene attachment",passed,count);
}
}
