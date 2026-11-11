#pragma once
#include "em1_navigation_data.h"
struct PPCContext;
namespace em2::em1 {
struct NativeNavigationGraph {
  uint32_t graph=0,sector=0;
  size_t nodes=0,edges=0,costs_requiring_native_half_rounding=0;
  std::vector<uint32_t> allocations;
};
// Engine-thread graph import/query ownership. These buffers are borrowed by
// the native object (DONT_DEALLOCATE); Release must precede memory shutdown.
// No world publication, mesh traversal or AI actor admission yet.
bool BuildNativeNavigationGraph(PPCContext&,uint8_t*,const NavigationSector&,NativeNavigationGraph&,std::string&);
bool FindNativeNavigationNode(PPCContext&,uint8_t*,const NativeNavigationGraph&,const std::array<float,3>&,uint32_t&,std::string&);
struct NativeNavigationPath {
  std::vector<uint32_t> nodes; // Start to goal, using original sector node indices.
  uint32_t iterations=0;
  uint8_t status=0; // Retail A* output status; 1=complete, 2=no path.
};
// A query-local, borrowed native streaming collection. Does not publish a world
// or invent links between sectors. Partial/limited searches are errors.
bool FindNativeNavigationPath(PPCContext&,uint8_t*,const NativeNavigationGraph&,uint32_t start,uint32_t goal,NativeNavigationPath&,std::string&);
void ReleaseNativeNavigationGraph(PPCContext&,uint8_t*,NativeNavigationGraph&);
// Complete HPD graph resource, retaining the original mesh/portal bytes through
// the source owner. Explicit engine-thread release precedes memory shutdown.
// This is an asset owner, not publication in the actor navigation world.
struct NativeNavigationAsset {
  NativeNavigationAsset()=default;
  NativeNavigationAsset(const NativeNavigationAsset&)=delete;
  NativeNavigationAsset& operator=(const NativeNavigationAsset&)=delete;
  NativeNavigationAsset(NativeNavigationAsset&&)=default;
  NativeNavigationAsset& operator=(NativeNavigationAsset&&)=delete;
  std::shared_ptr<const NavigationDatabase> source;
  std::vector<NativeNavigationGraph> graphs;
};
bool BuildNativeNavigationAsset(PPCContext&,uint8_t*,std::shared_ptr<const NavigationDatabase>,NativeNavigationAsset&,std::string&);
void ReleaseNativeNavigationAsset(PPCContext&,uint8_t*,NativeNavigationAsset&);
}
