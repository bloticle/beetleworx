#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace em2::em1 {
using NavigationPoint=std::array<float,3>;
struct NavigationSegment {NavigationPoint from{},to{};};
struct NavigationFloorPortal {NavigationSegment segment;uint32_t target_floor=0;};
struct NavigationUnboundLink {
  uint32_t source_offset=0;
  NavigationSegment segment;
  std::array<uint32_t,2> link_words{};
  NavigationPoint direction{};
  // Resolved only from an explicit HPD neighbor group; source words retained.
  uint32_t target_sector_index=UINT32_MAX,target_floor_index=UINT32_MAX;
};
struct NavigationPatch {NavigationPoint direction{};std::vector<NavigationPoint> points;};
struct NavigationFloor {
  uint32_t source_offset=0,source_size=0,x=0,z=0,layer=0;
  float minimum_height=0,maximum_height=0;
  float height_tolerance=0;
  std::array<uint8_t,80> header{};
  std::vector<NavigationSegment> segments;
  std::vector<NavigationFloorPortal> portals;
  std::vector<NavigationUnboundLink> unbound_links;
  std::vector<std::vector<NavigationPoint>> polylines;
  std::vector<NavigationPatch> patches;
};
struct NavigationFloorNeighbor {
  uint32_t sector=0,x=0,z=0,layer=0;
  // Mesh-relative offsets of explicitly listed local unbound-link records.
  std::vector<uint32_t> local_links;
};
struct NavigationRuntimeMesh {
  uint32_t sector=0,width=0,height=0;
  float inverse_cell_size=0,border_tolerance=0;
  std::array<float,4> bounds{}; // xmin,xmax,zmin,zmax
  std::vector<NavigationFloor> floors;
  std::vector<std::vector<uint32_t>> cell_floors;
  std::vector<NavigationFloorNeighbor> neighbors;
};
// Source geometry and references only. Does not reinterpret patches, invent
// triangles, infer walkability or fill missing cross-sector connections.
// Input is the complete original KynogonRuntimeMesh tail, including its footer.
bool ReadNavigationRuntimeMesh(std::span<const uint8_t>,NavigationRuntimeMesh&,std::string&);
// Original floor test42FED8/42FD48. Interior parity uses XZ; height is checked
// on edges. Callers must still select the correct floor layer/height range.
// This is not a complete world walkability, clearance or collision query.
bool NavigationFloorContainsPoint(const NavigationFloor&,const NavigationPoint&,float height_tolerance);
// Original42F8A8/42F98C: at most four nearby cells, ordered floor layers,
// inclusive stored Y ranges, and the floor's authored edge-height tolerance.
const NavigationFloor* FindNavigationFloor(const NavigationRuntimeMesh&,const NavigationPoint&);
struct NavigationDatabase;
struct NavigationFloorHandle {
  uint32_t sector_index=UINT32_MAX,floor_index=UINT32_MAX;
  bool operator==(const NavigationFloorHandle&)const=default;
};
struct NavigationPortalStep {
  NavigationFloorHandle floor;
  NavigationPoint point{},normal{};
};
struct NavigationPortalHistory {
  std::array<NavigationFloorHandle,16> floors{};
  uint32_t count=0;
};
// Original459CBC: directed XZ intersection; edge_point uses the edge's Y,
// move_point (optional) uses the movement segment's Y. Collinear is no hit.
bool NavigationDirectedIntersection(const NavigationSegment&,const NavigationSegment&,NavigationPoint& edge_point,NavigationPoint* move_point=nullptr);
// Original42CAE0 portal step. Not a complete wall/clearance/CanGo query.
// Neighbor targets must come from the database's explicit source bindings.
bool StepNavigationPortal(const NavigationDatabase&,NavigationFloorHandle,const NavigationSegment&,float unit_scale,NavigationPortalHistory&,NavigationPortalStep&);
}
