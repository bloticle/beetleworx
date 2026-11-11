#pragma once
#include "em1_navigation_mesh.h"
#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace em2::em1 {
class RvzSource;
struct NavigationSectorDeclaration {
  uint32_t id=0;
  std::array<float,6> bounds{}; // xmin,xmax,ymin,ymax,zmin,zmax
  uint32_t unknown=0,payload_size=0,payload_offset=0;
  std::array<uint32_t,2> cook{};
};
struct NavigationNode {
  uint32_t id=0;
  std::array<float,3> position{};
  std::array<uint8_t,20> metadata{};
  uint32_t first_edge=0; // Original one-based adjacency start, zero if empty.
};
struct NavigationEdge {uint32_t encoded_cost=0,source=0,target=0;};
struct NavigationSector {
  std::string source_path;
  uint32_t id=0;std::array<uint32_t,2> cook{};
  std::vector<NavigationNode> nodes;
  std::vector<NavigationEdge> edges;
  size_t mesh_offset=0;
  NavigationRuntimeMesh runtime_mesh;
  // Own complete original bytes, including runtime geometry, portals, external
  // links, cook metadata and footer. Graph decoding does not replace a mesh.
  std::vector<uint8_t> bytes;
};
struct NavigationDatabase {
  std::string source_path,source_archive,source_identity;
  std::vector<uint8_t> bytes;
  std::vector<NavigationSectorDeclaration> declarations;
  std::vector<NavigationSector> sectors;
};
// Transactional read/validation only. Never supplies an empty replacement for
// malformed/missing data, changes traversal flags, or invents walkable surfaces.
bool ReadNavigationHpd(std::span<const uint8_t>,std::vector<NavigationSectorDeclaration>&,std::string&);
bool ReadNavigationSector(std::span<const uint8_t>,NavigationSector&,std::string&);
bool LoadNavigationDatabase(std::shared_ptr<RvzSource>,std::string_view archive,std::string_view path,NavigationDatabase&,std::string&);
}
