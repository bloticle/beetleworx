#include "em1_navigation_mesh.h"
#include "em1_navigation_data.h"
#include "em1_gekko_math.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace em2::em1 {
namespace {
// Preserve separate single-precision PPC operations, including exact tests
// against the interpolated intersection. Do not contract multiply/add to FMA.
float Add(float a,float b){volatile float result=a+b;return result;}
float Sub(float a,float b){volatile float result=a-b;return result;}
float Mul(float a,float b){volatile float result=a*b;return result;}
float Div(float a,float b){volatile float result=a/b;return result;}
int Truncate(float value){if(!(value>=-2147483648.f && value<2147483648.f))return std::numeric_limits<int>::min();return static_cast<int>(value);}
bool Edge(const NavigationPoint& p,const NavigationSegment& segment,float tolerance,bool& parity){
  const auto& a=segment.from;const auto& b=segment.to;
  // Original outer routine's three fsel predicates (1.01 or0, sum>=3).
  if(p[2]<std::min(a[2],b[2]) || p[2]>std::max(a[2],b[2]) || p[0]>std::max(a[0],b[0]))return false;
  if(a[2]!=b[2]){
    if(a[0]==b[0]){
      if(p[0]==a[0])return std::abs(Sub(Add(a[1],Div(Mul(Sub(p[2],a[2]),Sub(b[1],a[1])),Sub(b[2],a[2]))),p[1]))<tolerance;
      if(p[2]>std::min(a[2],b[2]))parity=!parity;
      return false;
    }
    const auto dz=Sub(p[2],a[2]),span=Sub(b[2],a[2]);
    const auto x=Add(a[0],Div(Mul(dz,Sub(b[0],a[0])),span));
    if(p[0]>x)return false;
    if(p[0]==x)return std::abs(Sub(Add(a[1],Div(Mul(dz,Sub(b[1],a[1])),span)),p[1]))<tolerance;
    if(p[2]>std::min(a[2],b[2]))parity=!parity;
    return false;
  }
  if(p[0]<std::min(a[0],b[0]))return false;
  return std::abs(Sub(Add(a[1],Div(Mul(Sub(p[0],a[0]),Sub(b[1],a[1])),Sub(b[0],a[0]))),p[1]))<tolerance;
}
}
bool NavigationFloorContainsPoint(const NavigationFloor& floor,const NavigationPoint& point,float height_tolerance){
  if(!std::isfinite(height_tolerance) || height_tolerance<0)return false;
  for(const auto coordinate:point)if(!std::isfinite(coordinate))return false;
  bool parity=false;
  for(const auto& segment:floor.segments)if(Edge(point,segment,height_tolerance,parity))return true;
  for(const auto& portal:floor.portals)if(Edge(point,portal.segment,height_tolerance,parity))return true;
  for(const auto& link:floor.unbound_links)if(Edge(point,link.segment,height_tolerance,parity))return true;
  return parity;
}
const NavigationFloor* FindNavigationFloor(const NavigationRuntimeMesh& mesh,const NavigationPoint& point){
  for(const auto value:point)if(!std::isfinite(value))return nullptr;
  if(!mesh.width || !mesh.height || mesh.cell_floors.size()!=size_t(mesh.width)*mesh.height)return nullptr;
  const auto& b=mesh.bounds;const auto padding=mesh.border_tolerance;
  if(point[0]<Sub(b[0],padding) || point[0]>=Add(b[1],padding) || point[2]<Sub(b[2],padding) || point[2]>=Add(b[3],padding))return nullptr;
  const auto x=Mul(Sub(point[0],b[0]),mesh.inverse_cell_size),z=Mul(Sub(point[2],b[2]),mesh.inverse_cell_size),pad=Mul(padding,mesh.inverse_cell_size);
  const int max_x=int(mesh.width)-1,max_z=int(mesh.height)-1;
  const auto center=std::pair{std::clamp(Truncate(x),0,max_x),std::clamp(Truncate(z),0,max_z)};
  const auto low_x=std::max(Truncate(Sub(x,pad)),0),high_x=std::min(Truncate(Add(x,pad)),max_x);
  const auto low_z=std::max(Truncate(Sub(z,pad)),0),high_z=std::min(Truncate(Add(z,pad)),max_z);
  std::array<std::pair<int,int>,4> candidates{};size_t count=1;candidates[0]=center;
  for(int cx=low_x;cx<=high_x && count<4;++cx)for(int cz=low_z;cz<=high_z && count<4;++cz)if(std::pair{cx,cz}!=center)candidates[count++]={cx,cz};
  for(size_t i=0;i<count;++i){const auto [cx,cz]=candidates[i];
    for(const auto index:mesh.cell_floors[size_t(cx)*mesh.height+cz]){
      if(index>=mesh.floors.size())return nullptr;const auto& floor=mesh.floors[index];
      if(point[1]>=floor.minimum_height && point[1]<=floor.maximum_height && NavigationFloorContainsPoint(floor,point,floor.height_tolerance))return &floor;
    }
  }
  return nullptr;
}
bool NavigationDirectedIntersection(const NavigationSegment& movement,const NavigationSegment& edge,NavigationPoint& point,NavigationPoint* move_point){
  const auto& a=movement.from;const auto& b=movement.to;const auto& c=edge.from;const auto& d=edge.to;
  for(const auto* p:{&a,&b,&c,&d})for(auto v:*p)if(!std::isfinite(v))return false;
  if((a[0]<c[0] && b[0]<c[0] && a[0]<d[0] && b[0]<d[0]) ||
     (a[0]>c[0] && b[0]>c[0] && a[0]>d[0] && b[0]>d[0]) ||
     (a[2]<c[2] && b[2]<c[2] && a[2]<d[2] && b[2]<d[2]) ||
     (a[2]>c[2] && b[2]>c[2] && a[2]>d[2] && b[2]>d[2]))return false;
  const auto ex=Sub(d[0],c[0]),ez=Sub(d[2],c[2]),mz=Sub(b[2],a[2]),mx=Sub(b[0],a[0]);
  const auto p=Mul(mz,ex),q=Mul(mx,ez);if(Sub(p,q)<0)return false;
  const auto denominator=Sub(q,p);if(denominator==0)return false;
  const auto ox=Sub(a[0],c[0]),oz=Sub(a[2],c[2]);
  const auto t=Div(Sub(Mul(ex,oz),Mul(ez,ox)),denominator);if(t<0 || t>1)return false;
  const auto u=Div(Sub(Mul(mx,oz),Mul(mz,ox)),denominator);if(u<0 || u>1)return false;
  NavigationPoint hit={Add(a[0],Mul(t,mx)),Add(c[1],Mul(u,Sub(d[1],c[1]))),Add(a[2],Mul(t,mz))};
  point=hit;if(move_point){hit[1]=Add(a[1],Mul(t,Sub(b[1],a[1])));*move_point=hit;}return true;
}
bool StepNavigationPortal(const NavigationDatabase& database,NavigationFloorHandle source,const NavigationSegment& movement,float unit_scale,NavigationPortalHistory& history,NavigationPortalStep& output){
  auto floor_at=[&](NavigationFloorHandle handle)->const NavigationFloor*{
    if(handle.sector_index>=database.sectors.size())return nullptr;const auto& floors=database.sectors[handle.sector_index].runtime_mesh.floors;
    return handle.floor_index<floors.size()?&floors[handle.floor_index]:nullptr;
  };
  const auto* floor=floor_at(source);if(!floor || !std::isfinite(unit_scale) || unit_scale<=0 || history.count>16)return false;
  for(const auto* point:{&movement.from,&movement.to})for(auto value:*point)if(!std::isfinite(value))return false;
  NavigationPortalStep result{source,movement.to,{}};float nearest=std::numeric_limits<float>::max();
  auto distance=[&](const NavigationPoint& p){const auto z=Sub(p[2],movement.from[2]),x=Sub(p[0],movement.from[0]);return Add(Mul(z,z),Mul(x,x));};
  auto normal=[&](const NavigationSegment& edge){
    const auto x=Sub(edge.to[2],edge.from[2]),z=Sub(edge.from[0],edge.to[0]);
    const auto length=gekko_math::FastSqrt(Add(Mul(z,z),Mul(x,x)));
    return length==0?NavigationPoint{}:NavigationPoint{Div(x,length),0,Div(z,length)};
  };
  for(const auto& portal:floor->portals){NavigationPoint hit;
    if(!NavigationDirectedIntersection(movement,portal.segment,hit))continue;const auto cost=distance(hit);
    if(cost>=nearest)continue;nearest=cost;result.floor={source.sector_index,portal.target_floor};result.point=hit;result.normal=normal(portal.segment);
  }
  if(!floor->unbound_links.empty()){
    const auto dz=Sub(movement.to[2],movement.from[2]),dx=Sub(movement.to[0],movement.from[0]);
    const auto length=gekko_math::FastSqrt(Add(Mul(dz,dz),Mul(dx,dx)));
    if(length==0){output={source,movement.to,{}};return true;}
    const auto nx=Div(dx,length),nz=Div(dz,length);
    const auto blocked_bias=Mul(Mul(0.01f,Mul(0.01f,unit_scale)),unit_scale);
    float best=std::numeric_limits<float>::max(),bias=0;NavigationFloorHandle target=source;NavigationPoint hit{},hit_normal{};
    for(const auto& link:floor->unbound_links){
      if(link.direction[1]==-2.f)continue;NavigationPoint p;if(!NavigationDirectedIntersection(movement,link.segment,p))continue;
      const auto dot=Add(Mul(link.direction[2],nz),Mul(link.direction[0],nx));
      const NavigationFloorHandle destination{link.target_sector_index,link.target_floor_index};
      const bool blocked=dot<=link.direction[1] || !floor_at(destination);const auto penalty=blocked?blocked_bias:0.f,cost=distance(p);
      if(Add(cost,penalty)>=Add(best,bias))continue;best=cost;bias=penalty;target=blocked?NavigationFloorHandle{}:destination;hit=p;hit_normal=normal(link.segment);
    }
    if(best!=std::numeric_limits<float>::max() && best<nearest){
      result={target,hit,hit_normal};
      if(!floor_at(target)){history.count=0;output=result;return true;}
      nearest=best;
    }
  }
  if(nearest!=0){history.count=0;result.normal={};output=result;return true;}
  bool cycle=history.count>=16;
  for(uint32_t i=0;i<history.count && !cycle;++i)cycle=history.floors[i]==source;
  if(cycle){history.count=0;output={{},movement.from,{}};return false;}
  history.floors[history.count++]=source;result.normal={};output=result;return true;
}
}
