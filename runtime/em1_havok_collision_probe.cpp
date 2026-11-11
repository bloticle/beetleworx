#include "em1_havok_collision_probe.h"
#include "em1_havok_collision.h"
#include "em1_jigsaw_havok.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

PPC_EXTERN_FUNC(__imp__sub_82ECC850); // actual native in-place pack loader
PPC_EXTERN_FUNC(__imp__sub_8245EBC8); // collection castRay; hkBool hidden return

namespace em2::em1 {
namespace {
using V=std::array<double,3>;
V Sub(V a,V b){for(unsigned k=0;k<3;++k)a[k]-=b[k];return a;}
V Cross(V a,V b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
double Dot(V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
V Vertex(const std::array<float,3>& v){return {v[0],v[1],v[2]};}
bool Mapped(uint8_t* base,uint32_t address,size_t size,bool write){
  if(!base||address<4096||!size||uint64_t(address)+size>0x100000000ull)return false;
  auto at=reinterpret_cast<uintptr_t>(base+address);const auto end=at+size;
  while(at<end){MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto access=info.Protect&255;
    const bool writable=access==PAGE_READWRITE||access==PAGE_WRITECOPY||access==PAGE_EXECUTE_READWRITE||access==PAGE_EXECUTE_WRITECOPY;
    if(!writable&&(write||(access!=PAGE_READONLY&&access!=PAGE_EXECUTE_READ)))return false;
    const auto next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
    if(next<=at)return false;at=std::min(end,next);
  }return true;
}
struct Context {
  PPCContext& ctx;PPCContext saved;unsigned csr;DWORD last_error;
  explicit Context(PPCContext& c):ctx(c),saved(c),csr(_mm_getcsr()),last_error(GetLastError()){}
  void Reset(){ctx=saved;ctx.r1.u64=uint64_t(saved.r1.u32)-2048;}
  ~Context(){ctx=saved;_mm_setcsr(csr);SetLastError(last_error);}
};
double Reference(const AuthoredCollisionBody& b,V from,V to){
  const V direction=Sub(to,from);double nearest=1;
  for(const auto& p:b.parts)for(const auto& t:p.triangles){
    const auto a=Vertex(p.vertices[t[0]]),e1=Sub(Vertex(p.vertices[t[1]]),a),e2=Sub(Vertex(p.vertices[t[2]]),a);
    const auto h=Cross(direction,e2);const double det=Dot(e1,h);
    if(std::abs(det)<1e-12)continue;
    const auto s=Sub(from,a);const double u=Dot(s,h)/det;if(u<0||u>1)continue;
    const auto q=Cross(s,e1);const double v=Dot(direction,q)/det;if(v<0||u+v>1)continue;
    const double fraction=Dot(e2,q)/det;if(fraction>=0&&fraction<nearest)nearest=fraction;
  }return nearest;
}
bool SampleRay(const AuthoredCollisionBody& b,V& from,V& to){
  for(const auto& p:b.parts)for(const auto& t:p.triangles){
    const auto a=Vertex(p.vertices[t[0]]),v=Vertex(p.vertices[t[1]]),w=Vertex(p.vertices[t[2]]);
    auto n=Cross(Sub(v,a),Sub(w,a));const auto length=std::sqrt(Dot(n,n));if(length<1e-6)continue;
    for(unsigned k=0;k<3;++k){const auto centre=(a[k]+v[k]+w[k])/3;n[k]/=length;from[k]=centre+n[k];to[k]=centre-n[k];}
    return true;
  }return false;
}
bool ProbeLoadedShape(Context& context,uint8_t* base,uint32_t shape,uint32_t scratch,
    const AuthoredCollisionBody& body,NativeCollisionProof& proof,std::string& error){
  auto& ctx=context.ctx;
  if(!Mapped(base,shape,0x110,true)||PPC_LOAD_U32(shape)!=0x82027b2c){error="Loaded collision shape has an unaudited native class";return false;}
  const auto input=scratch,output=scratch+64,returned=scratch+160;
  auto StoreFloat=[&](uint32_t a,float f){PPC_STORE_U32(a,std::bit_cast<uint32_t>(f));};
    const auto table=PPC_LOAD_U32(shape);
    if(!Mapped(base,table,32,false)||PPC_LOAD_U32(table+28)!=0x8245ebc8){error="Native collection raycast ABI differs from audited image";return false;}
    ++proof.loaded_shapes;
    auto Cast=[&](V from,V to,bool expected_hit){
      std::memset(base+input,0,64);std::memset(base+output,0,96);base[returned]=0;
      for(unsigned k=0;k<3;++k){StoreFloat(input+k*4,float(from[k]));StoreFloat(input+16+k*4,float(to[k]));from[k]=float(from[k]);to[k]=float(to[k]);}
      StoreFloat(output+16,1);PPC_STORE_U32(output+20,UINT32_MAX);
      for(unsigned k=0;k<8;++k)PPC_STORE_U32(output+32+k*4,UINT32_MAX);
      const double reference=Reference(body,from,to);
      if(expected_hit!=(reference<1)){error="Independent authored triangle ray fixture is degenerate";return false;}
      context.Reset();ctx.r3.u32=returned;ctx.r4.u32=shape;ctx.r5.u32=input;ctx.r6.u32=output;
      __imp__sub_8245EBC8(ctx,base);
      const bool hit=base[returned]!=0;const float fraction=std::bit_cast<float>(PPC_LOAD_U32(output+16));
      if(hit!=expected_hit||!std::isfinite(fraction)||std::abs(double(fraction)-reference)>0.0001){
        error="Native collection raycast differs from authored triangle oracle: body="+body.name+" expected="+std::to_string(reference)+" actual="+std::to_string(fraction)+" hit="+std::to_string(hit);return false;
      }
      proof.largest_fraction_error=std::max(proof.largest_fraction_error,float(std::abs(double(fraction)-reference)));
      if(hit){if(PPC_LOAD_U32(output+32)==UINT32_MAX){error="Native hit did not return a child shape key";return false;}++proof.hit_rays;}else ++proof.miss_rays;
      return true;
    };
    V from{},to{};if(!SampleRay(body,from,to)){error="Authored body has no nondegenerate triangle";return false;}
    if(!Cast(from,to,true))return false;
    V outside={-1e4,-1e4,-1e4};for(const auto& p:body.parts)for(const auto& v:p.vertices)for(unsigned k=0;k<3;++k)outside[k]=std::max(outside[k],double(v[k])+16);
    auto end=outside;end[0]+=1;if(!Cast(outside,end,false))return false;
    REXLOG_INFO("[em1] collision native pack/ray passed body='{}' shape={:08X}",body.name,shape);
  return true;
}
}
size_t NativeCollisionProbeScratchSize(const AuthoredStaticCollision& source){
  size_t size=256;for(const auto& b:source.bodies){if(b.native_shape_pack.size()>64*1024*1024)return 0;size+=(b.native_shape_pack.size()+31)&~size_t(31);}return size;
}
bool ProbeNativeAuthoredCollision(PPCContext& ctx,uint8_t* base,uint32_t scratch,
    size_t scratch_size,const AuthoredStaticCollision& source,
    NativeCollisionProof& proof,std::string& error){
  Context context(ctx);proof={};error.clear();const auto required=NativeCollisionProbeScratchSize(source);
  constexpr uint32_t stack_window=32768;
  if(source.bodies.empty()||!required||scratch_size<required||(scratch&31)||!Mapped(base,scratch,required,true)||ctx.r1.u32<stack_window||!Mapped(base,ctx.r1.u32-stack_window,stack_window,true)||
      (uint64_t(scratch)<ctx.r1.u32&&uint64_t(scratch)+required>ctx.r1.u32-stack_window)){
    error="Native collision proof needs private aligned zero scratch and initialized game-thread stack";return false;
  }
  if(!std::all_of(base+scratch,base+scratch+required,[](uint8_t v){return v==0;})){
    error="Native collision proof refuses nonzero scratch";return false;
  }
  // Stock loader's default built-in registry must already exist. This helper
  // never changes a registry or suppresses native errors to make loading pass.
  if(!Mapped(base,0x833ccf18,4,false)||!PPC_LOAD_U32(0x833ccf18)){
    error="Native Havok built-in registry is not initialized";return false;
  }
  size_t offset=256;
  for(const auto& body:source.bodies){
    const auto& pack=body.native_shape_pack;if(pack.size()<208){error="Missing reconstructed native shape pack";return false;}
    const uint32_t address=scratch+uint32_t(offset);std::memcpy(base+address,pack.data(),pack.size());
    REXLOG_INFO("[em1] collision native pack loading body='{}' bytes={}",body.name,pack.size());
    context.Reset();ctx.r3.u32=address;ctx.r4.u32=uint32_t(pack.size());ctx.r5.u32=0;ctx.r6.u32=0;
    __imp__sub_82ECC850(ctx,base);const auto shape=ctx.r3.u32;
    if(!shape||shape<address||uint64_t(shape)+0x110>uint64_t(address)+pack.size()||!Mapped(base,shape,0x110,true)||PPC_LOAD_U32(shape)!=0x82027b2c){
      error="Native in-place loader did not return a StorageExtendedMeshShape";return false;
    }
    if(!ProbeLoadedShape(context,base,shape,scratch,body,proof,error))return false;
    offset+=(pack.size()+31)&~size_t(31);
  }
  return proof.loaded_shapes==source.bodies.size()&&proof.hit_rays==source.bodies.size()&&proof.miss_rays==source.bodies.size();
}
bool ProbeNativeLoadedCollisionShapes(PPCContext& ctx,uint8_t* base,uint32_t scratch,
    std::span<const uint32_t> shapes,const AuthoredStaticCollision& source,
    NativeCollisionProof& proof,std::string& error){
  Context scope(ctx);proof={};error.clear();
  if(shapes.size()!=source.bodies.size() || shapes.empty() || (scratch&31) || !Mapped(base,scratch,256,true) ||
      ctx.r1.u32<32768 || !Mapped(base,ctx.r1.u32-32768,32768,true) ||
      (uint64_t(scratch)<ctx.r1.u32 && uint64_t(scratch)+256>ctx.r1.u32-32768) ||
      std::any_of(shapes.begin(),shapes.end(),[&](uint32_t shape){return uint64_t(scratch)<uint64_t(shape)+0x110 && uint64_t(scratch)+256>shape;})){
    error="Loaded collision query requires matching owned shapes, private scratch and game-thread stack";return false;
  }
  for(size_t i=0;i<shapes.size();++i)if(!ProbeLoadedShape(scope,base,shapes[i],scratch,source.bodies[i],proof,error))return false;
  return proof.loaded_shapes==shapes.size() && proof.hit_rays==shapes.size() && proof.miss_rays==shapes.size();
}
bool ProbeNativeJigsawRays(PPCContext& ctx,uint8_t* base,uint32_t root,uint32_t scratch,
    const JigsawAggregateInfo& source,NativeCollisionProof& proof,std::string& error){
  Context scope(ctx);proof={};error.clear();
  if(source.triangles.empty() || (scratch&31) || !Mapped(base,scratch,256,true) ||
      !Mapped(base,root,64,true) || ctx.r1.u32<32768 || !Mapped(base,ctx.r1.u32-32768,32768,true)){
    error="MOPP ray probe requires authored triangle samples, owned root/private scratch and initialized stack";return false;
  }
  const auto table=PPC_LOAD_U32(root);
  if(table!=0x82025c28 || !Mapped(base,table,32,false)){error="MOPP shape vtable differs from audited retail ABI";return false;}
  const auto cast=PPC_LOAD_U32(table+28);
  if(cast<0x82000000 || cast>=0x83400000 || !Mapped(base,cast,4,false)){error="MOPP hkpShape raycast entry unavailable";return false;}
  const uint32_t input=scratch,output=scratch+64,returned=scratch+160;
  const auto collection=PPC_LOAD_U32(root+0x34);
  if(!Mapped(base,collection,0xf0,true) || PPC_LOAD_U32(collection)!=0x82027abc || !Mapped(base,PPC_LOAD_U32(collection),32,false)){error="Original MOPP child collection differs from audited retail ABI";return false;}
  const auto collection_cast=PPC_LOAD_U32(PPC_LOAD_U32(collection)+28);
  if(collection_cast<0x82000000 || collection_cast>=0x83400000 || !Mapped(base,collection_cast,4,false)){error="Original collection ray entry unavailable";return false;}
  REXLOG_INFO("[em1] original MOPP query ABI: root_table={:08X} cast={:08X} collection_table={:08X} collection_cast={:08X}",table,cast,PPC_LOAD_U32(collection),collection_cast);
  auto store=[&](uint32_t at,float value){PPC_STORE_U32(at,std::bit_cast<uint32_t>(value));};
  auto cast_ray=[&](V from,V to){
    std::memset(base+scratch,0,256);store(output+16,1);PPC_STORE_U32(output+20,UINT32_MAX);
    for(unsigned i=0;i<8;++i)PPC_STORE_U32(output+32+i*4,UINT32_MAX);
    for(unsigned i=0;i<3;++i){from[i]=float(from[i]);to[i]=float(to[i]);store(input+i*4,float(from[i]));store(input+16+i*4,float(to[i]));}
    const auto direction=Sub(to,from);double nearest=1;std::vector<uint32_t> keys;
    for(const auto& triangle:source.triangles){
      const auto a=Vertex(triangle.vertices[0]),e1=Sub(Vertex(triangle.vertices[1]),a),e2=Sub(Vertex(triangle.vertices[2]),a);
      const auto h=Cross(direction,e2);const double det=Dot(e1,h);if(std::abs(det)<1e-12)continue;
      const auto s=Sub(from,a);const double u=Dot(s,h)/det;if(u<0 || u>1)continue;
      const auto q=Cross(s,e1);const double v=Dot(direction,q)/det;if(v<0 || u+v>1)continue;
      const double fraction=Dot(e2,q)/det;if(fraction<0 || fraction>=1)continue;
      if(fraction<nearest-1e-6){nearest=fraction;keys.clear();}
      if(std::abs(fraction-nearest)<=1e-6)keys.push_back(triangle.key);
    }
    // Mixed collections also contain original convex children. Compare the
    // MOPP accelerator with that SAME unaccelerated native collection, without
    // using the collection as an in-game fallback or replacing the tree.
    scope.Reset();ctx.r3.u32=returned;ctx.r4.u32=collection;ctx.r5.u32=input;ctx.r6.u32=output;ctx.ctr.u64=collection_cast;
    PPC_CALL_INDIRECT_FUNC(collection_cast);
    const bool direct_hit=base[returned]!=0;const float direct_fraction=std::bit_cast<float>(PPC_LOAD_U32(output+16));const auto direct_key=PPC_LOAD_U32(output+32);
    std::array<uint32_t,8> direct_keys{};for(unsigned i=0;i<8;++i)direct_keys[i]=PPC_LOAD_U32(output+32+i*4);
    std::memset(base+output,0,96);store(output+16,1);PPC_STORE_U32(output+20,UINT32_MAX);for(unsigned i=0;i<8;++i)PPC_STORE_U32(output+32+i*4,UINT32_MAX);base[returned]=0;
    scope.Reset();ctx.r3.u32=returned;ctx.r4.u32=root;ctx.r5.u32=input;ctx.r6.u32=output;ctx.ctr.u64=cast;
    PPC_CALL_INDIRECT_FUNC(cast);
    const bool hit=base[returned]!=0;const float fraction=std::bit_cast<float>(PPC_LOAD_U32(output+16));
    // Original82451938 increments the key-stack level before traversal, then
    // stores its single-child container key0 at the outer level on a hit.
    const auto outer_key=PPC_LOAD_U32(output+32),key=PPC_LOAD_U32(output+36);
    bool hierarchy=true;for(unsigned i=1;i<7;++i)hierarchy=hierarchy && PPC_LOAD_U32(output+36+i*4)==direct_keys[i];
    // Traversal order may choose different COPLANAR source triangles. Accept
    // only when independent source intersections prove both keys at this hit.
    const bool source_tie=std::abs(double(direct_fraction)-nearest)<=1e-4 &&
        std::find(keys.begin(),keys.end(),direct_key)!=keys.end() && std::find(keys.begin(),keys.end(),key)!=keys.end();
    const double expected=source.convex_parts?direct_fraction:nearest;
    if(hit!=direct_hit || !std::isfinite(fraction) || !std::isfinite(direct_fraction) || fraction<0 || fraction>1 || direct_fraction<0 || direct_fraction>1 || std::abs(double(fraction)-direct_fraction)>1e-4 || (hit && (outer_key!=0 || !hierarchy || direct_keys[7]!=UINT32_MAX || (key!=direct_key && !source_tie))) ||
        (!source.convex_parts && (hit!=(nearest<1) || std::abs(double(fraction)-nearest)>1e-4 || (hit && std::find(keys.begin(),keys.end(),key)==keys.end())))){
      error="Native original MOPP ray mismatch: expected="+std::to_string(expected)+" actual="+std::to_string(fraction)+" direct_key="+std::to_string(direct_key)+" key="+std::to_string(key)+" next_key="+std::to_string(PPC_LOAD_U32(output+36))+" level="+std::to_string(PPC_LOAD_U32(output+64));return false;
    }
    proof.largest_fraction_error=std::max(proof.largest_fraction_error,float(std::abs(double(fraction)-expected)));
    if(hit)++proof.hit_rays;else ++proof.miss_rays;return true;
  };
  for(size_t selected:{size_t(0),source.triangles.size()/3,source.triangles.size()*2/3,source.triangles.size()-1}){
    const auto& triangle=source.triangles[selected];const auto a=Vertex(triangle.vertices[0]),b=Vertex(triangle.vertices[1]),c=Vertex(triangle.vertices[2]);
    auto normal=Cross(Sub(b,a),Sub(c,a));const auto length=std::sqrt(Dot(normal,normal));
    if(length<1e-6){error="Fixed source MOPP ray sample is degenerate";return false;}
    V from{},to{};for(unsigned i=0;i<3;++i){const auto centre=(a[i]+b[i]+c[i])/3;normal[i]/=length;from[i]=centre+normal[i];to[i]=centre-normal[i];}
    if(!cast_ray(from,to))return false;
  }
  V outside={-1e4,-1e4,-1e4};for(const auto& triangle:source.triangles)for(const auto& point:triangle.vertices)for(unsigned i=0;i<3;++i)outside[i]=std::max(outside[i],double(point[i])+16);
  auto end=outside;end[0]+=1;if(!cast_ray(outside,end))return false;
  proof.loaded_shapes=1;return true;
}
}
