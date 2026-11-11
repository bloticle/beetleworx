#include "em1_havok_collision_world.h"
#include "em1_havok_collision.h"
#include "em1_jigsaw_havok_probe.h"
#include "ppc_context.h"
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

PPC_EXTERN_FUNC(__imp__sub_82ECC850); // in-place native pack load/finish
PPC_EXTERN_FUNC(__imp__sub_8249B068); // rigid body Cinfo defaults
PPC_EXTERN_FUNC(__imp__sub_82474700); // normal native rigid body constructor
PPC_EXTERN_FUNC(__imp__sub_82391160); // quaternion from hkRotation
PPC_EXTERN_FUNC(__imp__sub_8238C398); // native addReference
PPC_EXTERN_FUNC(__imp__sub_8238C448); // native removeReference/deleting destructor
PPC_EXTERN_FUNC(__imp__sub_8246A600); // hkpWorld addEntity
PPC_EXTERN_FUNC(__imp__sub_8246A7C8); // hkpWorld removeEntity, hidden hkBool result
PPC_EXTERN_FUNC(__imp__sub_8246A1C0); // world markForWrite
PPC_EXTERN_FUNC(__imp__sub_8246A1F8); // world unmarkForWrite
PPC_EXTERN_FUNC(__imp__sub_8246B1F8); // native closest world raycast

namespace em2::em1 {
namespace {
bool Mapped(uint8_t* base,uint32_t address,size_t bytes,bool write=true){
  if(!base||address<4096||!bytes||uint64_t(address)+bytes>0x100000000ull)return false;
  auto at=reinterpret_cast<uintptr_t>(base+address);const auto end=at+bytes;
  while(at<end){MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto access=info.Protect&255;
    const bool writable=access==PAGE_READWRITE||access==PAGE_WRITECOPY||access==PAGE_EXECUTE_READWRITE||access==PAGE_EXECUTE_WRITECOPY;
    if(!writable&&(write||(access!=PAGE_READONLY&&access!=PAGE_EXECUTE_READ)))return false;
    const auto next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;if(next<=at)return false;at=std::min(end,next);
  }return true;
}
struct Context {
  PPCContext& ctx;PPCContext saved;unsigned csr;DWORD last_error;
  explicit Context(PPCContext& c):ctx(c),saved(c),csr(_mm_getcsr()),last_error(GetLastError()){}
  void Reset(){ctx=saved;ctx.r1.u64=uint64_t(saved.r1.u32)-2048;}
  ~Context(){ctx=saved;_mm_setcsr(csr);SetLastError(last_error);}
};
bool Stack(PPCContext& ctx,uint8_t* base){return ctx.r1.u32>=32768&&Mapped(base,ctx.r1.u32-32768,32768);}
struct Temporary {
  rex::memory::Memory& memory;uint32_t address;
  explicit Temporary(rex::memory::Memory& m):memory(m),address(m.SystemHeapAlloc(512,32)){}
  ~Temporary(){if(address)memory.SystemHeapFree(address);}
};
std::array<float,16> Compose(const std::array<float,16>& a,const std::array<float,16>& b){
  std::array<float,16> result{};result[15]=1;
  for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r)for(unsigned k=0;k<3;++k)result[c*4+r]+=a[k*4+r]*b[c*4+k];
  for(unsigned r=0;r<3;++r){result[12+r]=a[12+r];for(unsigned k=0;k<3;++k)result[12+r]+=a[k*4+r]*b[12+k];}return result;
}
void Float(uint8_t* base,uint32_t address,float value){PPC_STORE_U32(address,std::bit_cast<uint32_t>(value));}
// Exactly the retail allocation sequence preceding 82474700, e.g.827E6B60:
// TLS memory router -> heap interface+44 -> blockAlloc vtable+4, size544.
uint32_t AllocateBody(Context& scope,uint8_t* base){
  auto& ctx=scope.ctx;const auto pcr=scope.saved.r13.u32;
  if(!Mapped(base,pcr,4,false))return 0;const auto tls=PPC_LOAD_U32(pcr);
  if(!Mapped(base,tls,12,false))return 0;const auto router=PPC_LOAD_U32(tls+8);
  if(!Mapped(base,router,48,false))return 0;const auto heap=PPC_LOAD_U32(router+44);
  if(!Mapped(base,heap,4,false))return 0;const auto table=PPC_LOAD_U32(heap);
  if(!Mapped(base,table,12,false))return 0;const auto allocate=PPC_LOAD_U32(table+4);
  if(allocate<0x82200000||allocate>=0x83340000||(allocate&3)||!Mapped(base,allocate,4,false))return 0;
  scope.Reset();ctx.r3.u64=heap;ctx.r4.u64=544;PPC_CALL_INDIRECT_FUNC(allocate);
  const auto body=ctx.r3.u32;if(!Mapped(base,body,544))return 0;
  PPC_STORE_U16(body+4,544);return body;
}
bool WorldReady(uint8_t* base,uint32_t world){
  return Mapped(base,world,816)&&PPC_LOAD_U16(world+4)!=0&&PPC_LOAD_U32(world+148)==0&&PPC_LOAD_U32(world+152)==0;
}
struct WorldWrite {
  Context& scope;uint8_t* base;uint32_t world;
  WorldWrite(Context& s,uint8_t* b,uint32_t w):scope(s),base(b),world(w){scope.Reset();scope.ctx.r3.u64=world;__imp__sub_8246A1C0(scope.ctx,base);}
  ~WorldWrite(){scope.Reset();scope.ctx.r3.u64=world;__imp__sub_8246A1F8(scope.ctx,base);}
};
}
NativeCollisionAttachment::~NativeCollisionAttachment(){
  if(!objects.empty()||world_reference)REXLOG_ERROR("[em1] collision owner destroyed before explicit native release; guest buffers retained to avoid dangling native shapes");
}
uint32_t CurrentNativeCollisionWorld(uint8_t* base){
  if(!Mapped(base,0x833da7e4,4,false))return 0;const auto core=PPC_LOAD_U32(0x833da7e4);
  if(!Mapped(base,core,232,false))return 0;const auto manager=PPC_LOAD_U32(core+228);
  if(!Mapped(base,manager,8,false))return 0;const auto world=PPC_LOAD_U32(manager+4);
  return Mapped(base,world,816)?world:0;
}
bool ConstructNativeAuthoredCollision(PPCContext& ctx,uint8_t* base,
    const AuthoredStaticCollision& source,const std::array<float,16>& placement,
    NativeCollisionAttachment& state,std::string& error){
  Context scope(ctx);error.clear();
  if(!state.objects.empty()||state.world_reference||source.bodies.empty()||!IsRigidCollisionPose(placement)||!Stack(ctx,base)){
    error="Collision construction requires empty owner, rigid placement and initialized game-thread stack";return false;
  }
  auto* kernel=rex::kernel::kernel_state();if(!kernel||!kernel->memory()){
    error="Native collision allocation context unavailable";return false;
  }
  state.memory=kernel->memory();Temporary work(*state.memory);
  if(!work.address||!Mapped(base,work.address,512)){error="Native collision Cinfo scratch allocation failed";return false;}
  const uint32_t cinfo=work.address,rotation=cinfo+256;
  auto abort=[&](const std::string& reason){std::string cleanup;if(!ReleaseNativeAuthoredCollision(ctx,base,state,cleanup))error=reason+"; retained cleanup: "+cleanup;else error=reason;return false;};
  for(const auto& body:source.bodies){
    if(body.motion_type!=5||!IsRigidCollisionPose(body.transform,true)||body.native_shape_pack.size()<208||body.name.size()>65536||body.properties.size()>65536)
      return abort("Unsupported authored static body pose/metadata: "+body.name);
    std::vector<uint8_t> fallback_pack;
    std::span<const uint8_t> selected_pack=body.native_shape_pack;
    bool use_acceleration=body.accelerated;
    if(body.accelerated){
      ConvertedJigsawAggregate check;check.pack=body.native_shape_pack;
      check.info.triangle_parts=uint32_t(body.parts.size());check.info.key_bits=body.key_bits;
      check.info.mopp_bytes=uint32_t(body.original_mopp_code.size());
      const size_t budget=(std::min)(size_t(24),size_t(4096)/body.parts.size());
      for(const auto& part:body.parts){const auto count=(std::min)(budget,part.triangles.size());
        for(size_t i=0;i<count;++i){const auto& triangle=part.triangles[i*part.triangles.size()/count];JigsawTriangleProof ray;
          for(unsigned j=0;j<3;++j)ray.vertices[j]=part.vertices[triangle[j]];
          check.info.triangles.push_back(ray);
        }
      }
      const auto bytes=NativeJigsawProbeScratchSize(check);const auto scratch=state.memory->SystemHeapAlloc(uint32_t(bytes),32);
      if(!scratch||!Mapped(base,scratch,bytes)){if(scratch)state.memory->SystemHeapFree(scratch);return abort("MOPP parity scratch allocation failed");}
      std::memset(base+scratch,0,bytes);NativeJigsawProof proof;std::string why;
      const bool valid=ProbeNativeJigsawAggregate(ctx,base,scratch,bytes,check,proof,why,&body);state.memory->SystemHeapFree(scratch);
      if(!valid){
        std::string fallback_error;
        if(!BuildUnacceleratedCollisionPack(body,fallback_pack,fallback_error))return abort("Collision body fallback failed: "+fallback_error);
        selected_pack=fallback_pack;use_acceleration=false;
        REXLOG_WARN("[em1] collision acceleration rejected for body '{}' only; other bodies retain independent proofs: {}",body.name,why);
      }else REXLOG_INFO("[em1] authored static MOPP parity: body='{}' rays={} parts={} fraction_error={}",body.name,proof.compared_rays,proof.triangle_parts_covered.size(),proof.largest_fraction_error);
    }
    const auto pack_bytes=(selected_pack.size()+31)&~size_t(31);
    const auto properties_bytes=body.properties.size()*16;
    const auto name_offset=pack_bytes+properties_bytes;
    const auto total=(name_offset+body.name.size()+1+31)&~size_t(31);
    if(total>64*1024*1024)return abort("Native collision retained buffer exceeds limit");
    const auto pack=state.memory->SystemHeapAlloc(uint32_t(total),32);
    if(!pack||!Mapped(base,pack,total)){if(pack)state.memory->SystemHeapFree(pack);return abort("Native collision retained allocation failed");}
    state.objects.push_back({0,pack,uint32_t(total),body.name});auto& owner=state.objects.back();
    std::memset(base+pack,0,total);std::memcpy(base+pack,selected_pack.data(),selected_pack.size());
    scope.Reset();ctx.r3.u64=pack;ctx.r4.u64=selected_pack.size();ctx.r5.u64=0;ctx.r6.u64=0;
    __imp__sub_82ECC850(ctx,base);const auto shape=ctx.r3.u32;
    if(shape<pack||uint64_t(shape)+(use_acceleration?0x40:0x110)>uint64_t(pack)+pack_bytes||PPC_LOAD_U16(shape+4)!=0)
      return abort("Native static shape load or pack ownership contract changed");
    const auto mesh=use_acceleration?PPC_LOAD_U32(shape+0x34):shape;
    if(mesh<pack||uint64_t(mesh)+0x110>uint64_t(pack)+pack_bytes||PPC_LOAD_U32(mesh)!=0x82027b2c)
      return abort("Native static mesh child or ownership contract changed");
    const auto pose=Compose(placement,body.transform);std::memset(base+cinfo,0,512);
    scope.Reset();ctx.r3.u64=cinfo;__imp__sub_8249B068(ctx,base);
    PPC_STORE_U32(cinfo,body.filter);PPC_STORE_U32(cinfo+4,shape);base[cinfo+0xb4]=5;base[cinfo+0xc8]=body.quality_type;
    Float(base,cinfo+0xa0,body.friction);Float(base,cinfo+0xa8,body.restitution);
    // Preserve native response type from the authored HK7 material header.
    base[cinfo+0xc4]=body.source_material[0];
    for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r)Float(base,rotation+c*16+r*4,pose[c*4+r]);
    for(unsigned k=0;k<3;++k)Float(base,cinfo+0x10+k*4,pose[12+k]);
    scope.Reset();ctx.r3.u64=cinfo+0x20;ctx.r4.u64=rotation;__imp__sub_82391160(ctx,base);
    const auto allocated=AllocateBody(scope,base);if(!allocated)return abort("Native Havok body allocation failed");
    owner.body=allocated;
    scope.Reset();ctx.r3.u64=allocated;ctx.r4.u64=cinfo;__imp__sub_82474700(ctx,base);
    if(ctx.r3.u32!=allocated||PPC_LOAD_U32(allocated)!=0x82030384||PPC_LOAD_U32(allocated+8)||PPC_LOAD_U32(allocated+16)!=shape||PPC_LOAD_U16(allocated+6)!=1)
      return abort("Native rigid body constructor ownership/layout differs from audited image");
    PPC_STORE_U32(allocated+0xc,body.user_data);
    const auto name=pack+uint32_t(name_offset);std::memcpy(base+name,body.name.c_str(),body.name.size()+1);PPC_STORE_U32(allocated+0x78,name);
    if(!body.properties.empty()){
      const auto properties=pack+uint32_t(pack_bytes);
      for(size_t j=0;j<body.properties.size();++j)for(unsigned k=0;k<4;++k)PPC_STORE_U32(properties+uint32_t(j)*16+k*4,body.properties[j][k]);
      PPC_STORE_U32(allocated+0x7c,properties);PPC_STORE_U32(allocated+0x80,uint32_t(body.properties.size()));PPC_STORE_U32(allocated+0x84,0x80000000u|uint32_t(body.properties.size()));
    }
    // The fixed-motion constructor uses exactly the Cinfo position/quaternion;
    // no render-node transform or guessed collision offset is introduced.
    for(unsigned k=0;k<3;++k)if(std::abs(std::bit_cast<float>(PPC_LOAD_U32(allocated+0x120+k*4))-pose[12+k])>0.0001)
      return abort("Native fixed body pose differs from original composed placement");
    REXLOG_INFO("[em1] constructed authored static body '{}' native={:08X} shape={:08X} pack={:08X} position={},{},{}",body.name,allocated,shape,pack,pose[12],pose[13],pose[14]);
  }
  return true;
}
bool AdmitNativeAuthoredCollision(PPCContext& ctx,uint8_t* base,uint32_t world,
    NativeCollisionAttachment& state,std::string& error){
  Context scope(ctx);error.clear();
  if(!Stack(ctx,base)||state.objects.empty()||state.world_reference||world!=CurrentNativeCollisionWorld(base)||!WorldReady(base,world)){
    error="Collision admission requires current unlocked native world and constructed private bodies";return false;
  }
  for(const auto& owner:state.objects)if(!Mapped(base,owner.body,544)||PPC_LOAD_U32(owner.body+8)){
    error="Collision body is invalid or already belongs to a world";return false;
  }
  scope.Reset();ctx.r3.u64=world;__imp__sub_8238C398(ctx,base);state.world=world;state.world_reference=true;
  WorldWrite lock(scope,base,world);
  for(const auto& owner:state.objects){
    scope.Reset();ctx.r3.u64=world;ctx.r4.u64=owner.body;ctx.r5.u64=0;__imp__sub_8246A600(ctx,base);
    if(ctx.r3.u32!=owner.body||PPC_LOAD_U32(owner.body+8)!=world){error="Native world did not immediately admit authored collision; owner retained for explicit cleanup";return false;}
  }
  REXLOG_INFO("[em1] admitted {} original authored static collision bodies into native world {:08X}",state.objects.size(),world);return true;
}
bool ProbeNativeCollisionFloorRay(PPCContext& ctx,uint8_t* base,
    const NativeCollisionAttachment& state,std::string& error){
  Context scope(ctx);error.clear();
  if(!Stack(ctx,base)||!state.memory||!state.world_reference||!WorldReady(base,state.world)){
    error="Authored floor ray requires live admitted collision and an unlocked native world";return false;
  }
  const auto main=std::find_if(state.objects.begin(),state.objects.end(),[](const auto& owner){return owner.name=="COLLISION";});
  if(main==state.objects.end()||!Mapped(base,main->body,544)||PPC_LOAD_U32(main->body+8)!=state.world){
    error="Original courtyard main collision body is not admitted";return false;
  }
  Temporary work(*state.memory);if(!work.address||!Mapped(base,work.address,512)){error="Native floor ray scratch allocation failed";return false;}
  const auto input=work.address,output=input+64;std::memset(base+input,0,512);
  constexpr float x=51.41127014f,z=5.21368694f,spawn_y=-.89998168f;
  constexpr float from_y=spawn_y+.2f,to_y=spawn_y-2.f;
  // Independent authored triangle-surface result, including original body pose;
  // audit artifact courtyard-collision-spawn-audit.json, main triangle4234.
  constexpr double expected_y=-.9846437325866865;
  Float(base,input,x);Float(base,input+4,from_y);Float(base,input+8,z);
  Float(base,input+16,x);Float(base,input+20,to_y);Float(base,input+24,z);
  // Native input: collection-filter enable byte32=0; filterInfo+36=0.
  Float(base,output+16,1);PPC_STORE_U32(output+20,UINT32_MAX);
  for(unsigned k=0;k<8;++k)PPC_STORE_U32(output+32+k*4,UINT32_MAX);
  {
    WorldWrite lock(scope,base,state.world);
    scope.Reset();ctx.r3.u64=state.world;ctx.r4.u64=input;ctx.r5.u64=output;
    __imp__sub_8246B1F8(ctx,base);
  }
  const auto collidable=PPC_LOAD_U32(output+80);
  const auto fraction=std::bit_cast<float>(PPC_LOAD_U32(output+16));
  if(!std::isfinite(fraction)||fraction<0||fraction>1){error="Native world ray returned an invalid fraction";return false;}
  const double y=double(from_y)+(double(to_y)-from_y)*fraction;
  if(!collidable||fraction==1){error="Native world ray missed the admitted authored courtyard floor";return false;}
  if(collidable!=main->body+16){
    const bool nearer=y>expected_y+.001;
    error=std::string(nearer?"Existing native world geometry occludes authored floor":"Ray missed authored floor and hit unrelated native world geometry")+
      "; collidable="+std::to_string(collidable)+" worldY="+std::to_string(y)+" expectedY="+std::to_string(expected_y);
    return false;
  }
  if(std::abs(y-expected_y)>.001){error="Native authored floor world-space hit differs from independent original triangle result: "+std::to_string(y);return false;}
  REXLOG_INFO("[em1] original courtyard floor native world ray passed: body={:08X} collidable={:08X} child={} y={} expected={} fraction={}",
    main->body,collidable,PPC_LOAD_U32(output+32),y,expected_y,fraction);return true;
}
bool SetNativeAuthoredCollisionActive(PPCContext& ctx,uint8_t* base,
    NativeCollisionAttachment& state,bool active,std::string& error){
  Context scope(ctx);error.clear();
  if(!Stack(ctx,base)||!state.memory||!state.world_reference||state.objects.empty()||!WorldReady(base,state.world)){
    error="Collision activation requires a retained owner and an unlocked native world";return false;
  }
  Temporary work(*state.memory);if(!work.address||!Mapped(base,work.address,512)){error="Collision activation scratch unavailable";return false;}
  // Validate the complete ownership closure before changing any body.
  for(const auto& object:state.objects){
    if(!Mapped(base,object.body,544)||PPC_LOAD_U32(object.body)!=0x82030384){error="Collision activation body type changed";return false;}
    const auto world=PPC_LOAD_U32(object.body+8);
    if(world && world!=state.world){error="Collision activation body belongs to another world";return false;}
  }
  WorldWrite lock(scope,base,state.world);
  for(const auto& object:state.objects){const auto world=PPC_LOAD_U32(object.body+8);
    if(active && !world){
      scope.Reset();ctx.r3.u64=state.world;ctx.r4.u64=object.body;ctx.r5.u64=0;__imp__sub_8246A600(ctx,base);
      if(ctx.r3.u32!=object.body||PPC_LOAD_U32(object.body+8)!=state.world){error="Native collision reactivation did not complete";return false;}
    }else if(!active && world){
      base[work.address]=0;scope.Reset();ctx.r3.u64=work.address;ctx.r4.u64=state.world;ctx.r5.u64=object.body;__imp__sub_8246A7C8(ctx,base);
      if(!base[work.address]||PPC_LOAD_U32(object.body+8)){error="Native collision deactivation did not complete";return false;}
    }
  }
  return true;
}
bool ReleaseNativeAuthoredCollision(PPCContext& ctx,uint8_t* base,
    NativeCollisionAttachment& state,std::string& error){
  Context scope(ctx);error.clear();if(state.objects.empty()&&!state.world_reference){state.memory=nullptr;return true;}
  if(!Stack(ctx,base)||!state.memory){error="Collision cleanup needs live runtime and original game-thread context";return false;}
  Temporary work(*state.memory);if(!work.address||!Mapped(base,work.address,512)){error="Collision removal scratch allocation failed";return false;}
  if(state.world_reference){
    if(!WorldReady(base,state.world)){error="Retained native world is unmapped or has pending critical operations; retry before owner teardown";return false;}
    WorldWrite lock(scope,base,state.world);
    for(const auto& owner:state.objects){
      if(!owner.body)continue;if(!Mapped(base,owner.body,544)||PPC_LOAD_U32(owner.body)!=0x82030384){error="Retained collision body became unmapped or changed type";return false;}
      const auto current=PPC_LOAD_U32(owner.body+8);if(!current)continue;
      if(current!=state.world){error="Collision body moved into an unexpected world; backing storage retained";return false;}
      base[work.address]=0;scope.Reset();ctx.r3.u64=work.address;ctx.r4.u64=state.world;ctx.r5.u64=owner.body;
      __imp__sub_8246A7C8(ctx,base);
      if(!base[work.address]||PPC_LOAD_U32(owner.body+8)){error="Native collision removal did not complete; backing storage retained";return false;}
    }
  }
  for(auto& owner:state.objects){
    if(owner.body){if(!Mapped(base,owner.body,544)||PPC_LOAD_U32(owner.body)!=0x82030384||PPC_LOAD_U32(owner.body+8)||PPC_LOAD_U16(owner.body+6)!=1){error="Unexpected retained body references; do not free its backing shape";return false;}
      scope.Reset();ctx.r3.u64=owner.body;__imp__sub_8238C448(ctx,base);owner.body=0;}
    if(owner.pack){state.memory->SystemHeapFree(owner.pack);owner.pack=0;}
  }
  state.objects.clear();if(state.world_reference){scope.Reset();ctx.r3.u64=state.world;__imp__sub_8238C448(ctx,base);}
  state.world=0;state.world_reference=false;state.memory=nullptr;
  REXLOG_INFO("[em1] authored collision bodies removed/released before pack buffers; native world reference released");return true;
}
}
