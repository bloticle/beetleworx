#include "em1_native_navigation.h"
#include "ppc_context.h"
#include <rex/kernel/kernel_state.h>
#include <windows.h>
#include <immintrin.h>
#include <bit>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
PPC_EXTERN_FUNC(__imp__sub_823556A8);
PPC_EXTERN_FUNC(__imp__sub_82355620);
PPC_EXTERN_FUNC(__imp__sub_82355F50);
PPC_EXTERN_FUNC(__imp__sub_823564E8);
PPC_EXTERN_FUNC(__imp__sub_82358F30);
PPC_EXTERN_FUNC(__imp__sub_82359908);
namespace em2::em1 {
void ReleaseNativeNavigationAsset(PPCContext& ctx,uint8_t* base,NativeNavigationAsset& asset){
  for(auto at=asset.graphs.rbegin();at!=asset.graphs.rend();++at)ReleaseNativeNavigationGraph(ctx,base,*at);
  asset.graphs.clear();asset.source.reset();
}
bool BuildNativeNavigationAsset(PPCContext& ctx,uint8_t* base,std::shared_ptr<const NavigationDatabase> source,NativeNavigationAsset& output,std::string& error){
  if(!source || source->sectors.empty() || output.source || !output.graphs.empty()){
    error="Invalid native HPD asset ownership request";return false;
  }
  NativeNavigationAsset candidate;candidate.source=std::move(source);candidate.graphs.reserve(candidate.source->sectors.size());
  for(const auto& sector:candidate.source->sectors){
    NativeNavigationGraph graph;
    if(!BuildNativeNavigationGraph(ctx,base,sector,graph,error)){ReleaseNativeNavigationAsset(ctx,base,candidate);return false;}
    candidate.graphs.push_back(std::move(graph));
  }
  output.source=std::move(candidate.source);output.graphs.swap(candidate.graphs);error.clear();return true;
}
namespace {
struct Context {PPCContext& ctx;PPCContext saved;DWORD error=GetLastError();unsigned csr=_mm_getcsr();Context(PPCContext& c):ctx(c),saved(c){ctx.r1.u64-=32768;}~Context(){ctx=saved;SetLastError(error);_mm_setcsr(csr);}};
void Check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void CheckNativeLayout(uint8_t* base){
  // Native hkClass initializers832EFD20 and edge/node reflection declarations.
  Check(PPC_LOAD_U32(0x833C369C+8)==0x4C && PPC_LOAD_U32(0x833C369C+28)==7,"Native explicit-cost graph schema differs");
  constexpr uint16_t offsets[]={8,20,32,44,56,68,72};const auto fields=PPC_LOAD_U32(0x833C369C+24);
  for(unsigned i=0;i<7;++i)Check(PPC_LOAD_U16(fields+i*24+18)==offsets[i],"Native navigation field offset differs");
  Check(PPC_LOAD_U32(0x833C360C+8)==8 && PPC_LOAD_U32(0x833C363C+8)==8,"Native graph node/edge schema differs");
}
}
void ReleaseNativeNavigationGraph(PPCContext& ctx,uint8_t* base,NativeNavigationGraph& owner){
  Context restore(ctx);auto* kernel=rex::kernel::kernel_state();
  if(owner.graph){ctx.r3.u64=owner.graph;ctx.r4.u64=0;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(owner.graph)));}
  if(kernel && kernel->memory())for(auto at=owner.allocations.rbegin();at!=owner.allocations.rend();++at)kernel->memory()->SystemHeapFree(*at);
  owner={};
}
bool BuildNativeNavigationGraph(PPCContext& ctx,uint8_t* base,const NavigationSector& source,NativeNavigationGraph& output,std::string& error){
  if(!base || output.graph || !output.allocations.empty() || ctx.r1.u32<65536){error="Invalid native navigation graph ownership/context";return false;}
  Context restore(ctx);NativeNavigationGraph owner;
  try {
    auto* kernel=rex::kernel::kernel_state();Check(kernel && kernel->memory(),"Native navigation memory unavailable");CheckNativeLayout(base);
    Check(source.nodes.size()<=65536 && source.edges.size()<=0x0FFFFFFF,"Navigation graph exceeds native index/storage limits");
    auto allocate=[&](size_t bytes){if(!bytes)return uint32_t(0);Check(bytes<=UINT32_MAX,"Native navigation allocation overflow");
      const auto at=kernel->memory()->SystemHeapAlloc(uint32_t(bytes),16);Check(at!=0,"Native navigation allocation failed");owner.allocations.push_back(at);std::memset(base+at,0,bytes);return at;};
    owner.graph=allocate(0x4C);owner.sector=source.id;owner.nodes=source.nodes.size();owner.edges=source.edges.size();
    PPC_STORE_U16(owner.graph+4,0x4C);PPC_STORE_U16(owner.graph+6,1);
    for(uint32_t offset:{8u,20u,32u,44u,56u})PPC_STORE_U32(owner.graph+offset+8,0x80000000);
    ctx.r3.u64=owner.graph;ctx.r4.u64=1;__imp__sub_823556A8(ctx,base);
    const auto positions=allocate(source.nodes.size()*16),nodes=allocate(source.nodes.size()*8),edges=allocate(source.edges.size()*8);
    auto array=[&](uint32_t offset,uint32_t data,size_t count){PPC_STORE_U32(owner.graph+offset,data);PPC_STORE_U32(owner.graph+offset+4,uint32_t(count));PPC_STORE_U32(owner.graph+offset+8,uint32_t(count)|0x80000000);};
    array(8,positions,source.nodes.size());array(20,nodes,source.nodes.size());array(32,edges,source.edges.size());
    std::vector<uint32_t> counts(source.nodes.size());for(const auto& edge:source.edges){Check(edge.source<counts.size() && edge.target<counts.size(),"Native graph edge outside source nodes");++counts[edge.source];}
    for(size_t i=0;i<source.nodes.size();++i){const auto& node=source.nodes[i];
      for(unsigned axis=0;axis<3;++axis){Check(std::isfinite(node.position[axis]),"Nonfinite native graph position");PPC_STORE_U32(positions+uint32_t(i)*16+axis*4,std::bit_cast<uint32_t>(node.position[axis]));}
      Check((counts[i]!=0)==(node.first_edge!=0),"Native graph adjacency missing");
      PPC_STORE_U32(nodes+uint32_t(i)*8,node.first_edge?node.first_edge-1:0);PPC_STORE_U32(nodes+uint32_t(i)*8+4,counts[i]);
    }
    for(size_t i=0;i<source.edges.size();++i){const auto& edge=source.edges[i];Check((edge.encoded_cost&0x80000000)!=0,"Unsupported source graph cost encoding");
      const auto cost=float(edge.encoded_cost&0x7FFFFFFF)/16.f;const auto bits=std::bit_cast<uint32_t>(cost);
      const uint16_t half=uint16_t(bits>>16);if((bits&65535)!=0)++owner.costs_requiring_native_half_rounding;
      PPC_STORE_U16(edges+uint32_t(i)*8,half);PPC_STORE_U16(edges+uint32_t(i)*8+2,0);PPC_STORE_U32(edges+uint32_t(i)*8+4,edge.target);
    }
    PPC_STORE_U32(owner.graph+68,source.id);PPC_STORE_U32(owner.graph+72,0);
    output=std::move(owner);error.clear();return true;
  }catch(const std::exception& e){error=e.what();ReleaseNativeNavigationGraph(ctx,base,owner);return false;}
}
bool FindNativeNavigationNode(PPCContext& ctx,uint8_t* base,const NativeNavigationGraph& owner,const std::array<float,3>& point,uint32_t& output,std::string& error){
  if(!owner.graph || ctx.r1.u32<65536){error="Native navigation graph/context unavailable";return false;}
  for(const auto value:point)if(!std::isfinite(value)){error="Invalid native navigation query coordinate";return false;}
  Context restore(ctx);auto* kernel=rex::kernel::kernel_state();if(!kernel || !kernel->memory()){error="Native graph query memory unavailable";return false;}
  const auto scratch=kernel->memory()->SystemHeapAlloc(16,16);if(!scratch){error="Native graph query scratch unavailable";return false;}
  std::memset(base+scratch,0,16);for(unsigned axis=0;axis<3;++axis)PPC_STORE_U32(scratch+axis*4,std::bit_cast<uint32_t>(point[axis]));
  ctx.r3.u64=owner.graph;ctx.r4.u64=scratch;__imp__sub_82355620(ctx,base);const auto result=ctx.r3.u32;kernel->memory()->SystemHeapFree(scratch);
  if(result!=UINT32_MAX && result>=owner.nodes){error="Native graph query returned an invalid node";return false;}
  output=result;error.clear();return true;
}
bool FindNativeNavigationPath(PPCContext& ctx,uint8_t* base,const NativeNavigationGraph& owner,uint32_t start,uint32_t goal,NativeNavigationPath& output,std::string& error){
  if(!base || !owner.graph || ctx.r1.u32<65536 || start>=owner.nodes || goal>=owner.nodes || owner.nodes>32767){error="Invalid native navigation path context/indices";return false;}
  Context restore(ctx);auto* kernel=rex::kernel::kernel_state();
  if(!kernel || !kernel->memory()){error="Native navigation path memory unavailable";return false;}
  // Retail FindGraphPath82359908 takes a streaming collection, a 1C-byte
  // FindGraphPathInput and an hkArray<uint32> followed by A* output parameters.
  // The retail scratch helper leaves size=0 for caller-provided buffers;
  // FindGraphPath then reads that size as available search capacity. Let the
  // native allocator own its two scratch arrays so their sizes are correct.
  const uint32_t capacity=uint32_t(owner.nodes)+1;
  const auto align16=[](uint32_t n){return (n+15)&~15u;};
  const uint32_t open_offset=align16(0x80+capacity*4),open_bytes=align16(capacity*8);
  const uint32_t state_offset=open_offset+open_bytes,state_bytes=align16(512+capacity*18);
  const auto storage=kernel->memory()->SystemHeapAlloc(open_offset,16);
  if(!storage){error="Native navigation path scratch allocation failed";return false;}
  std::memset(base+storage,0,open_offset);
  bool collection_initialized=false;
  auto cleanup=[&]{if(collection_initialized){ctx.r3.u64=storage;__imp__sub_823564E8(ctx,base);}kernel->memory()->SystemHeapFree(storage);};
  try {
    CheckNativeLayout(base);
    Check(PPC_LOAD_U32(0x833C37A4+8)==0x1C && PPC_LOAD_U32(0x833C3774+8)==0x18 && PPC_LOAD_U32(0x833C3D2C+8)==0x1C,"Native navigation query schema differs");
    PPC_STORE_U16(storage+4,0x1C);PPC_STORE_U16(storage+6,1);
    ctx.r3.u64=storage;ctx.r4.u64=1;__imp__sub_82355F50(ctx,base);collection_initialized=true;
    // Retail temporary collections borrow references; destructor82355F60
    // explicitly skips release when isTemporary is set. The array itself is
    // DONT_DEALLOCATE and freed here with the complete scratch allocation.
    PPC_STORE_U8(storage+8,1);PPC_STORE_U32(storage+0x10,storage+0x20);
    PPC_STORE_U32(storage+0x14,1);PPC_STORE_U32(storage+0x18,0x80000001);
    PPC_STORE_U32(storage+0x28,owner.graph);PPC_STORE_U32(storage+0x34,UINT32_MAX);
    const auto input=storage+0x40,result=storage+0x60;
    ctx.r3.u64=input;__imp__sub_82358F30(ctx,base);
    PPC_STORE_U32(input,start);PPC_STORE_U32(input+4,goal);
    PPC_STORE_U32(input+12,open_bytes);PPC_STORE_U32(input+16,state_bytes);
    PPC_STORE_U32(result,storage+0x80);PPC_STORE_U32(result+8,capacity|0x80000000);
    ctx.r3.u64=storage;ctx.r4.u64=input;ctx.r5.u64=result;__imp__sub_82359908(ctx,base);
    NativeNavigationPath path;path.iterations=PPC_LOAD_U32(result+16);path.status=PPC_LOAD_U8(result+24);
    const auto count=PPC_LOAD_U32(result+4);
    Check(PPC_LOAD_U32(result)==storage+0x80 && count<=capacity,"Native navigation path storage changed unexpectedly");
    Check(path.status==1 || path.status==2,"Native navigation path search did not complete");
    for(uint32_t i=0;i<count;++i){const auto node=PPC_LOAD_U32(storage+0x80+i*4);Check(node<owner.nodes,"Native navigation path leaves its source sector");path.nodes.push_back(node);}
    if(path.status==1){
      Check(!path.nodes.empty(),"Native navigation path has no nodes");
      if(path.nodes.front()==goal && path.nodes.back()==start)std::reverse(path.nodes.begin(),path.nodes.end());
      Check(path.nodes.front()==start && path.nodes.back()==goal,"Native navigation path endpoints differ");
    }else Check(path.nodes.empty(),"Native no-path result contains a route");
    cleanup();output=std::move(path);error.clear();return true;
  }catch(const std::exception& e){error=e.what();cleanup();return false;}
}
}
