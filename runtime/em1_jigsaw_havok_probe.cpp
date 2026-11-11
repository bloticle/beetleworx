#include "em1_jigsaw_havok_probe.h"
#include "em1_triangle_key_oracle.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>

PPC_EXTERN_FUNC(__imp__sub_82ECC850);
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
void Need(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct Hit {bool hit=false;float fraction=1;std::array<uint32_t,8> keys{};std::array<float,3> normal{};};
} // namespace
size_t NativeJigsawProbeScratchSize(const ConvertedJigsawAggregate& source) {
 return source.pack.size()>=208 && source.pack.size()<=64*1024*1024 ? 512+((source.pack.size()+31)&~size_t(31)):0;
}
bool ProbeNativeJigsawAggregate(PPCContext& ctx,uint8_t* base,uint32_t scratch,
    size_t scratch_size,const ConvertedJigsawAggregate& source,
    NativeJigsawProof& proof,std::string& error,const AuthoredCollisionBody* authored_geometry) {
 Context scope(ctx);proof={};error.clear();NativeJigsawProof prepared;
 try {
  const auto required=NativeJigsawProbeScratchSize(source);constexpr uint32_t stack_window=32768;
  Need(required && scratch_size>=required && !(scratch&31) && Mapped(base,scratch,required,true) &&
       ctx.r1.u32>=stack_window && Mapped(base,ctx.r1.u32-stack_window,stack_window,true) &&
       !(uint64_t(scratch)<ctx.r1.u32 && uint64_t(scratch)+required>ctx.r1.u32-stack_window),"Jigsaw probe needs private aligned zero scratch and initialized native stack");
  Need(std::all_of(base+scratch,base+scratch+required,[](uint8_t v){return v==0;}),"Jigsaw probe refuses nonzero scratch");
  Need(source.info.triangle_parts && source.info.triangle_parts<=1024 && source.info.convex_parts<=1024 &&
       !source.info.triangles.empty() && source.info.triangles.size()<=4096 && source.info.convex_vertices.size()<=4096 &&
       source.info.key_bits>1 && source.info.key_bits<=31,"Jigsaw focused native proof corpus exceeds bounds");
  Need(Mapped(base,0x833ccf18,4,false) && PPC_LOAD_U32(0x833ccf18),"Native Havok built-in registry is not initialized");
  const auto address=scratch+512;const uint64_t end=uint64_t(address)+source.pack.size();
  std::memcpy(base+address,source.pack.data(),source.pack.size());
  scope.Reset();ctx.r3.u32=address;ctx.r4.u32=uint32_t(source.pack.size());ctx.r5.u32=0;ctx.r6.u32=0;
  __imp__sub_82ECC850(ctx,base);const auto root=ctx.r3.u32;
  auto Inside=[&](uint32_t p,size_t bytes){return p>=address && uint64_t(p)+bytes<=end && Mapped(base,p,bytes,false);};
  Need(root && Inside(root,0x40),"Native Jigsaw loader returned no in-place MOPP root");
  const auto mesh=PPC_LOAD_U32(root+0x34),code=PPC_LOAD_U32(root+0x14);
  Need(Inside(mesh,0xf0) && Inside(code,0x30) && PPC_LOAD_U32(code+0x24)==source.info.mopp_bytes,"Native Jigsaw graph/code count differs");
  Need(PPC_LOAD_U32(mesh+0xb4)==source.info.key_bits && PPC_LOAD_U32(mesh+0xbc)==source.info.triangle_parts &&
       PPC_LOAD_U32(mesh+0xc8)==source.info.convex_parts,"Native Jigsaw collection/keyspace differs");
  auto Method=[&](uint32_t shape){const auto table=PPC_LOAD_U32(shape);Need(Mapped(base,table,32,false),"Invalid native Jigsaw shape vtable");
   const auto method=PPC_LOAD_U32(table+28);Need(method>=0x822f0000 && method<0x83350000 && !(method&3) && PPC_LOOKUP_FUNC(base,method),"Invalid native Jigsaw castRay method");return method;};
  const auto mopp_method=Method(root),mesh_method=Method(mesh);
  Need(mesh_method==0x8245ebc8,"Native collection castRay ABI differs from audited image");
  REXLOG_INFO("[em1] Jigsaw native proof loaded root={:08X} child={:08X} ray={:08X}/{:08X} bytes={}",root,mesh,mopp_method,mesh_method,source.pack.size());
  ++prepared.loaded_aggregates;
  const auto input=scratch,output=scratch+64,returned=scratch+192;
  auto Float=[&](uint32_t p){return std::bit_cast<float>(PPC_LOAD_U32(p));};
  auto Store=[&](uint32_t p,float value){PPC_STORE_U32(p,std::bit_cast<uint32_t>(value));};
  auto Cast=[&](uint32_t shape,uint32_t method,V from,V to){
   std::memset(base+input,0,64);std::memset(base+output,0,96);base[returned]=0;
   for(unsigned k=0;k<3;++k){Store(input+k*4,float(from[k]));Store(input+16+k*4,float(to[k]));}
   Store(output+16,1);PPC_STORE_U32(output+20,UINT32_MAX);for(unsigned k=0;k<8;++k)PPC_STORE_U32(output+32+k*4,UINT32_MAX);
   scope.Reset();ctx.r3.u32=returned;ctx.r4.u32=shape;ctx.r5.u32=input;ctx.r6.u32=output;PPC_CALL_INDIRECT_FUNC(method);
   Hit hit;hit.hit=base[returned]!=0;hit.fraction=Float(output+16);for(unsigned k=0;k<8;++k)hit.keys[k]=PPC_LOAD_U32(output+32+k*4);
   for(unsigned k=0;k<3;++k)hit.normal[k]=Float(output+k*4);
   Need(std::isfinite(hit.fraction) && hit.fraction>=0 && hit.fraction<=1,"Native Jigsaw ray returned invalid fraction");return hit;
  };
  std::set<uint32_t> triangles,convex;
  auto Compare=[&](V from,V to,bool must_miss=false){
   const auto reference=Cast(mesh,mesh_method,from,to),accelerated=Cast(root,mopp_method,from,to);++prepared.compared_rays;
   Need(reference.hit==accelerated.hit,"Original MOPP VM hit/miss disagrees with native child collection");
   const auto difference=std::abs(reference.fraction-accelerated.fraction);prepared.largest_fraction_error=std::max(prepared.largest_fraction_error,difference);
   Need(difference<=0.0001f,"Original MOPP VM fraction disagrees with native child collection");
   Need(!must_miss || !reference.hit,"Jigsaw outside ray unexpectedly intersects native collection");
   if(reference.hit){
    for(unsigned k=0;k<3;++k)Need(std::isfinite(reference.normal[k]) && std::isfinite(accelerated.normal[k]) && std::abs(reference.normal[k]-accelerated.normal[k])<=0.0001f,"Original MOPP VM normal disagrees with native child collection");
    // Retail MOPP castRay 82451938 increments hierarchy depth before its VM
    // call and writes child selector zero after decrementing it (82451A00..
    // 82451A20). The collection's full key chain therefore follows that zero.
    std::array<uint32_t,8> expected{};expected[0]=0;
    std::copy_n(reference.keys.begin(),7,expected.begin()+1);
    bool verified_tie=false;
    if(expected!=accelerated.keys && authored_geometry && !source.info.convex_parts &&
        authored_geometry->parts.size()==source.info.triangle_parts && authored_geometry->key_bits==source.info.key_bits &&
        accelerated.keys[0]==0 && reference.keys[0]!=UINT32_MAX && reference.keys[7]==UINT32_MAX &&
        std::equal(reference.keys.begin()+1,reference.keys.begin()+7,accelerated.keys.begin()+2)){
      std::array<float,3> actual_from{},actual_to{};
      for(unsigned k=0;k<3;++k){actual_from[k]=Float(input+k*4);actual_to[k]=Float(input+16+k*4);}
      verified_tie=AreEquivalentTriangleKeyHits(*authored_geometry,reference.keys[0],accelerated.keys[1],actual_from,actual_to,accelerated.fraction);
      if(verified_tie)++prepared.verified_triangle_ties;
    }
    if(expected!=accelerated.keys && !verified_tie)REXLOG_WARN("[em1] MOPP key mismatch: mesh={:08X}/{:08X}/{:08X}/{:08X} mopp={:08X}/{:08X}/{:08X}/{:08X} fraction={}/{}",reference.keys[0],reference.keys[1],reference.keys[2],reference.keys[3],accelerated.keys[0],accelerated.keys[1],accelerated.keys[2],accelerated.keys[3],reference.fraction,accelerated.fraction);
    Need((expected==accelerated.keys || verified_tie) && reference.keys[0]!=UINT32_MAX && reference.keys[7]==UINT32_MAX,"Original MOPP VM full shape-key chain disagrees with native child collection");
    const auto key=reference.keys[0],part=(key&0x7fffffffu)>>(32-source.info.key_bits);
    if(key&0x80000000u){Need(part<source.info.convex_parts,"Native convex key part exceeds authored keyspace");convex.insert(part);}
    else{Need(part<source.info.triangle_parts,"Native triangle key part exceeds authored keyspace");triangles.insert(part);}++prepared.hit_rays;
   }else ++prepared.miss_rays;
  };
  V outside={-1e4,-1e4,-1e4};
  for(const auto& triangle:source.info.triangles){
   const auto a=Vertex(triangle.vertices[0]),b=Vertex(triangle.vertices[1]),c=Vertex(triangle.vertices[2]);auto normal=Cross(Sub(b,a),Sub(c,a));const auto length=std::sqrt(Dot(normal,normal));
   for(const auto& v:triangle.vertices)for(unsigned k=0;k<3;++k)outside[k]=std::max(outside[k],double(v[k])+64);
   if(length<1e-8)continue;V center{};for(unsigned k=0;k<3;++k){center[k]=(a[k]+b[k]+c[k])/3;normal[k]/=length;}
   for(const auto distance:{0.001,0.05,0.5,2.0}){V from{},to{};for(unsigned k=0;k<3;++k){from[k]=center[k]+normal[k]*distance;to[k]=center[k]-normal[k]*distance;}Compare(from,to);Compare(to,from);}
  }
  // Decode the actual loaded convex arrays, independently compare the original
  // four-vector coordinates, then generate rays in each subpart's authored
  // rotation/translation. No guessed AABB substitution or triangle-only gate.
  const auto parts=PPC_LOAD_U32(mesh+0xc4);Need(!source.info.convex_parts || Inside(parts,uint64_t(source.info.convex_parts)*64),"Native convex part array range");
  std::set<uint32_t> checked_convex;size_t original_vertex=0;
  for(uint32_t part=0;part<source.info.convex_parts;++part){
   const auto p=parts+part*64,count=PPC_LOAD_U32(p+24),children=PPC_LOAD_U32(p+20);
   Need(count && count<=1024 && Inside(children,uint64_t(count)*4),"Native convex child array range");
   std::array<double,4> q{},translation{};for(unsigned k=0;k<4;++k){q[k]=Float(p+32+k*4);translation[k]=Float(p+48+k*4);Need(std::isfinite(q[k]) && std::isfinite(translation[k]),"Nonfinite native convex transform");}
   auto Transform=[&](V v){const V xyz={q[0],q[1],q[2]};auto t=Cross(xyz,v);for(auto& f:t)f*=2;const auto rotated=Cross(xyz,t);for(unsigned k=0;k<3;++k)v[k]+=q[3]*t[k]+rotated[k]+translation[k];return v;};
   for(uint32_t child=0;child<count;++child){
    const auto shape=PPC_LOAD_U32(children+child*4);Need(Inside(shape,112),"Native convex shape range");const auto vertices=PPC_LOAD_U32(shape+76),groups=PPC_LOAD_U32(shape+68),data=PPC_LOAD_U32(shape+64);
    Need(vertices && vertices<=4096 && uint64_t(vertices)+3>=uint64_t(groups)*4 && vertices<=uint64_t(groups)*4 && Inside(data,uint64_t(groups)*48),"Native convex vertex range");
    const bool first=checked_convex.insert(shape).second;std::vector<V> points;V center{};
    for(uint32_t i=0;i<vertices;++i){V local{};for(unsigned k=0;k<3;++k){local[k]=Float(data+(i/4)*48+k*16+(i%4)*4);Need(std::isfinite(local[k]),"Nonfinite native convex vertex");
      if(first){Need(original_vertex+i<source.info.convex_vertices.size() && float(local[k])==source.info.convex_vertices[original_vertex+i][k],"Native convex vertex differs from authored source");}}
     auto world=Transform(local);points.push_back(world);for(unsigned k=0;k<3;++k){center[k]+=world[k]/vertices;outside[k]=std::max(outside[k],world[k]+64);}}
    if(first)original_vertex+=vertices;
    double radius=1;for(const auto& point:points){const auto d=Sub(point,center);radius=std::max(radius,std::sqrt(Dot(d,d))*2);}
    for(unsigned axis=0;axis<3;++axis){auto from=center,to=center;from[axis]+=radius;to[axis]-=radius;Compare(from,to);Compare(to,from);}
    for(const auto& point:points){auto direction=Sub(point,center);const auto length=std::sqrt(Dot(direction,direction));if(length<1e-8)continue;V from{};for(unsigned k=0;k<3;++k)from[k]=point[k]+direction[k]*0.5/length;Compare(from,center);}
   }
  }
  Need(original_vertex==source.info.convex_vertices.size() && checked_convex.size()==source.info.convex_shapes,"Native convex closure coverage differs");
  auto miss=outside;miss[0]+=1;Compare(outside,miss,true);
  Need(triangles.size()==source.info.triangle_parts && convex.size()==source.info.convex_parts,"Native ray proof did not cover every authored triangle and convex part");
  prepared.triangle_parts_covered.assign(triangles.begin(),triangles.end());prepared.convex_parts_covered.assign(convex.begin(),convex.end());proof=std::move(prepared);
  REXLOG_INFO("[em1] Jigsaw original MOPP/native child proof passed rays={} triangle_parts={} convex_parts={} error={} source_verified_triangle_ties={}",proof.compared_rays,proof.triangle_parts_covered.size(),proof.convex_parts_covered.size(),proof.largest_fraction_error,proof.verified_triangle_ties);return true;
 }catch(const std::exception& exception){proof={};error=exception.what();return false;}
}
} // namespace em2::em1
