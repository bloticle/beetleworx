#pragma once
#include "em1_havok_collision.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace em2::em1 {
namespace triangle_key_detail {
using D3=std::array<double,3>;
inline constexpr double kWorldTolerance=1e-4;
inline constexpr double kPlaneTolerance=1e-6;
inline constexpr double kFractionTolerance=1e-4;
inline constexpr double kPairFractionTolerance=1e-7;
inline D3 Sub(const D3& a,const D3& b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline D3 Cross(const D3& a,const D3& b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline double Dot(const D3& a,const D3& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline bool Finite(const std::array<float,3>& v){return std::all_of(v.begin(),v.end(),[](float x){return std::isfinite(x);});}
inline D3 Double(const std::array<float,3>& v){return {v[0],v[1],v[2]};}
struct Triangle {std::array<std::array<float,3>,3> source{};std::array<D3,3> vertex{};D3 normal{};double area=0;std::array<double,3> altitude{};uint32_t filter=0,user_data=0;};
inline bool ReadTriangle(const AuthoredCollisionBody& body,uint32_t key,Triangle& out){
  if(body.key_bits<2 || body.key_bits>31 || (key&0x80000000u) || body.parts.empty() ||
      uint64_t(body.parts.size())>(uint64_t(1)<<(body.key_bits-1)))return false;
  const unsigned index_bits=32-body.key_bits;
  const uint32_t part=key>>index_bits,index=key&uint32_t((uint64_t(1)<<index_bits)-1);
  if(part>=body.parts.size())return false;
  const auto& p=body.parts[part];
  if(p.triangles.size()>(uint64_t(1)<<index_bits) || index>=p.triangles.size() || p.triangle_filters.size()!=p.triangles.size())return false;
  for(unsigned i=0;i<3;++i){const auto v=p.triangles[index][i];if(v>=p.vertices.size() || !Finite(p.vertices[v]))return false;
    out.source[i]=p.vertices[v];out.vertex[i]=Double(out.source[i]);}
  const auto e1=Sub(out.vertex[1],out.vertex[0]),e2=Sub(out.vertex[2],out.vertex[0]);
  const auto n=Cross(e1,e2);out.area=std::sqrt(Dot(n,n));
  const auto size=std::sqrt(Dot(e1,e1))*std::sqrt(Dot(e2,e2));
  if(!std::isfinite(out.area) || !std::isfinite(size) || !(size>0) || !(out.area>1e-12*size))return false;
  for(unsigned i=0;i<3;++i){out.normal[i]=n[i]/out.area;
    const auto edge=Sub(out.vertex[(i+2)%3],out.vertex[(i+1)%3]);const auto length=std::sqrt(Dot(edge,edge));
    if(!(length>0) || !std::isfinite(length))return false;out.altitude[i]=out.area/length;}
  out.filter=p.triangle_filters[index];out.user_data=p.user_data;return true;
}
inline bool Intersection(const Triangle& t,const D3& from,const D3& direction,double length,double& fraction){
  const auto e1=Sub(t.vertex[1],t.vertex[0]),e2=Sub(t.vertex[2],t.vertex[0]);
  const auto p=Cross(direction,e2);const auto determinant=Dot(e1,p);
  // Reject grazing/degenerate solves rather than widening equivalence.
  if(!std::isfinite(determinant) || std::abs(determinant)<=1e-12*t.area*length)return false;
  const auto delta=Sub(from,t.vertex[0]),q=Cross(delta,e1);
  const double u=Dot(delta,p)/determinant,v=Dot(direction,q)/determinant;
  fraction=Dot(e2,q)/determinant;
  if(!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(fraction) || fraction<0 || fraction>1)return false;
  // Barycentric slop is measured in WORLD units (triangle altitudes), so a
  // tiny triangle or long edge does not gain an arbitrary barycentric margin.
  // Actual native f32 edge rays can land a few ulps across a shared edge.
  return (1-u-v)*t.altitude[0]>=-kWorldTolerance && u*t.altitude[1]>=-kWorldTolerance && v*t.altitude[2]>=-kWorldTolerance;
}
}

// Narrow proof of a coplanar edge/duplicate primitive tie. Caller must retain
// its native hit/fraction/normal/hierarchy checks and retain the MOPP's key.
// This never grants equivalence based on adjacent numbers or welding words.
inline bool AreEquivalentTriangleKeyHits(const AuthoredCollisionBody& body,uint32_t keyA,uint32_t keyB,
    const std::array<float,3>& from,const std::array<float,3>& to,float fraction){
  using namespace triangle_key_detail;
  if(keyA==keyB || !Finite(from) || !Finite(to) || !std::isfinite(fraction) || fraction<0 || fraction>1)return false;
  Triangle a,b;if(!ReadTriangle(body,keyA,a) || !ReadTriangle(body,keyB,b) || a.filter!=b.filter || a.user_data!=b.user_data)return false;
  if(Dot(a.normal,b.normal)<1-1e-8)return false;
  for(const auto& p:a.vertex)if(std::abs(Dot(Sub(p,b.vertex[0]),b.normal))>kPlaneTolerance)return false;
  for(const auto& p:b.vertex)if(std::abs(Dot(Sub(p,a.vertex[0]),a.normal))>kPlaneTolerance)return false;
  std::array<D3,3> shared{};unsigned count=0;
  for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)if(a.source[i]==b.source[j]){shared[count++]=a.vertex[i];break;}
  if(count!=2 && count!=3)return false;
  const auto origin=Double(from),direction=Sub(Double(to),origin);const auto length=std::sqrt(Dot(direction,direction));
  if(!(length>0) || !std::isfinite(length))return false;
  double ta=0,tb=0;
  if(!Intersection(a,origin,direction,length,ta) || !Intersection(b,origin,direction,length,tb) ||
      std::abs(ta-fraction)>kFractionTolerance || std::abs(tb-fraction)>kFractionTolerance || std::abs(ta-tb)>kPairFractionTolerance)return false;
  D3 hit{};for(unsigned i=0;i<3;++i)hit[i]=origin[i]+double(fraction)*direction[i];
  if(std::abs(Dot(Sub(hit,a.vertex[0]),a.normal))>kWorldTolerance || std::abs(Dot(Sub(hit,b.vertex[0]),b.normal))>kWorldTolerance)return false;
  if(count==2){
    const auto edge=Sub(shared[1],shared[0]),offset=Sub(hit,shared[0]);const auto length2=Dot(edge,edge);
    if(!(length2>0))return false;
    const auto projection=Dot(offset,edge)/length2;
    if(projection<0 || projection>1)return false; // Segment, not infinite edge.
    D3 distance{};for(unsigned i=0;i<3;++i)distance[i]=offset[i]-projection*edge[i];
    if(Dot(distance,distance)>kWorldTolerance*kWorldTolerance)return false;
  }
  return true;
}

inline bool TestTriangleKeyOracle(std::string& error){
  try{
    const auto need=[](bool ok,const char* text){if(!ok)throw std::runtime_error(text);};
    AuthoredCollisionBody quad;quad.key_bits=12;AuthoredCollisionPart part;
    part.vertices={{{0,0,0}},{{1,0,0}},{{0,1,0}},{{1,1,0}}};part.triangles={{{0,1,2}},{{2,1,3}}};part.triangle_filters={0x18001,0x18001};quad.parts.push_back(part);
    const std::array<float,3> from{.5f,.5f,1},to{.5f,.5f,-1};
    const auto tie=[&](const AuthoredCollisionBody& b,uint32_t a=0,uint32_t c=1){return AreEquivalentTriangleKeyHits(b,a,c,from,to,.5f);};
    need(tie(quad),"Coplanar shared-edge tie rejected");need(AreEquivalentTriangleKeyHits(quad,1,0,from,to,.5f),"Key order changed equivalence");
    need(!tie(quad,0,0) && !tie(quad,0,2) && !tie(quad,0,0x80000001u) && !tie(quad,0,1u<<20),"Invalid/same/convex/part key accepted");
    auto bad=quad;bad.key_bits=1;need(!tie(bad),"Invalid key bits accepted");bad.key_bits=32;need(!tie(bad),"Oversized key bits accepted");
    bad=quad;bad.parts[0].triangle_filters[1]^=1;need(!tie(bad),"Different triangle materials accepted");
    bad=quad;bad.parts[0].triangle_filters.pop_back();need(!tie(bad),"Incomplete triangle filters accepted");
    bad=quad;bad.parts[0].triangles[1][2]=99;need(!tie(bad),"Bad vertex index accepted");
    bad=quad;bad.parts[0].vertices[3][2]=.01f;need(!tie(bad),"Noncoplanar adjacent faces accepted");
    bad=quad;bad.parts[0].triangles[1]={2,3,1};need(!tie(bad),"Opposite winding accepted");
    bad=quad;bad.parts[0].triangles[1]={0,0,1};need(!tie(bad),"Degenerate source triangle accepted");
    bad=quad;bad.parts[0].vertices[1][0]=std::numeric_limits<float>::quiet_NaN();need(!tie(bad),"Nonfinite source vertex accepted");
    need(!AreEquivalentTriangleKeyHits(quad,0,1,{.25f,.25f,1},{.25f,.25f,-1},.5f),"Interior hit on only one face accepted");
    need(!AreEquivalentTriangleKeyHits(quad,0,1,{2,-1,1},{2,-1,-1},.5f),"Infinite shared-edge extension accepted");
    need(!AreEquivalentTriangleKeyHits(quad,0,1,{3,3,1},{3,3,-1},.5f),"Nonhit ray accepted");
    need(!AreEquivalentTriangleKeyHits(quad,0,1,from,to,.6f),"Wrong native fraction accepted");
    need(!AreEquivalentTriangleKeyHits(quad,0,1,from,from,.5f),"Zero-length ray accepted");
    need(!AreEquivalentTriangleKeyHits(quad,0,1,from,to,std::numeric_limits<float>::infinity()),"Nonfinite fraction accepted");
    need(!AreEquivalentTriangleKeyHits(quad,0,1,{.5f,.5f,1},{.5f,.5f,2},.5f),"Plane behind ray accepted");
    AuthoredCollisionBody duplicate=quad;duplicate.parts[0].triangles[1]={2,0,1};
    need(AreEquivalentTriangleKeyHits(duplicate,0,1,{.2f,.3f,1},{.2f,.3f,-1},.5f),"Duplicate triangle vertices rejected");
    duplicate.parts[0].triangles[1]={0,2,1};need(!AreEquivalentTriangleKeyHits(duplicate,0,1,{.2f,.3f,1},{.2f,.3f,-1},.5f),"Reversed duplicate accepted");
    auto split=quad;split.parts.push_back(part);split.parts[1].vertices={part.vertices[2],part.vertices[1],part.vertices[3]};
    split.parts[1].triangles={{{0,1,2}}};split.parts[1].triangle_filters={0x18001};
    need(tie(split,0,1u<<20),"Exact shared source coordinates across parts rejected");
    split.parts[1].user_data=7;need(!tie(split,0,1u<<20),"Different source part userdata accepted");
    for(const float scale:{.125f,16.f}){auto transformed=quad;
      for(auto& v:transformed.parts[0].vertices){v[0]=100+v[0]*scale;v[1]=-200+v[1]*scale;v[2]=30+v[2]*scale;}
      need(AreEquivalentTriangleKeyHits(transformed,0,1,{100+.5f*scale,-200+.5f*scale,30+scale},
          {100+.5f*scale,-200+.5f*scale,30-scale},.5f),"Scaled/translated edge tie rejected");
    }
    // Original portal source coordinates, keys and reported native fraction;
    // the f32 endpoints below are constructed around the source shared edge,
    // not claimed to be the captured live ray. Tests float endpoint rounding.
    const auto portal=[&](const std::array<std::array<float,3>,4>& vertices,uint32_t a,uint32_t b,float fraction){
      AuthoredCollisionBody body;body.key_bits=12;AuthoredCollisionPart p;p.vertices.assign(vertices.begin(),vertices.end());
      p.triangles.resize(b+1);p.triangle_filters.resize(b+1,0);p.triangles[a]={0,1,2};p.triangles[b]={2,1,3};body.parts.push_back(p);
      triangle_key_detail::Triangle t;need(triangle_key_detail::ReadTriangle(body,a,t),"Portal triangle invalid");
      std::array<float,3> start{},end{};for(unsigned i=0;i<3;++i){const double midpoint=(double(vertices[1][i])+vertices[2][i])*.5;
        start[i]=float(midpoint+t.normal[i]*2*fraction);end[i]=float(midpoint-t.normal[i]*2*(1-fraction));}
      need(AreEquivalentTriangleKeyHits(body,a,b,start,end,fraction),"Real portal coordinate edge tie rejected");
    };
    portal({{{-9.1749849319458f,5.8337860107421875f,-69.43666076660156f},
             {-8.986068725585938f,5.8337860107421875f,-69.19898986816406f},
             {-9.1749849319458f,6.1081461906433105f,-69.43666076660156f},
             {-8.986068725585938f,6.1081461906433105f,-69.19898986816406f}}},36,37,.36853525f);
    portal({{{7.655353546142578f,1.7977956533432007f,-76.92748260498047f},
             {7.655353546142578f,1.5341671705245972f,-76.92748260498047f},
             {7.239743709564209f,1.7977956533432007f,-76.06390380859375f},
             {7.239743709564209f,1.5341671705245972f,-76.06390380859375f}}},527,528,.23900187f);
    error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
}
