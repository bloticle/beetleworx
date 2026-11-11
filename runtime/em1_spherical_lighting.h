#pragma once
#include "em1_preview_scene.h"
#include "em1_nif_container.h"
#include "em1_gekko_math.h"
#include <bit>
#include <cmath>

namespace em2::em1 {
using LightVector=std::array<float,3>;
using LightRotation=std::array<LightVector,3>;
using LightCoefficients=std::array<LightVector,9>;
namespace lighting_detail {
// Gekko frsqrte lookup, retaining the double estimate through fmuls.
// Reference: bluethinner pose_blend.rs / spherical_lighting.rs (EM1 REL).
inline double Estimate(float value) {return gekko_math::ReciprocalSqrtEstimate(value);}

inline std::pair<float,LightVector> Distance(LightVector v) {
  const float squared=std::fma(v[2],v[2],v[0]*v[0])+v[1]*v[1];
  nif::Check(std::isfinite(squared),"SH distance overflow");
  if(squared==0)return {};
  const double estimate=Estimate(squared);const auto bits=std::bit_cast<uint64_t>(estimate);
  const double operand=std::bit_cast<double>(((bits>>28)+((bits>>27)&1))<<28);
  const float square=float(estimate*operand),half=float(estimate*.5);
  const float inverse=std::fma(-square,squared,3.f)*half,distance=squared*inverse;
  if(distance<=1e-6f)return {};
  for(auto& f:v)f*=inverse;return {distance,v};
}
inline float Cosine(float degrees) {return float(std::cos(double(((3.1415927410125732f*degrees)/180.f)*.5f)));}
inline std::array<float,9> Project(LightVector d) {
  const auto [x,y,z]=d;const float negative=-.2731371223926544f*z;
  return {.282094806432724f,.3257350027561188f*y,.3257350027561188f*z,.3257350027561188f*x,
    .1365685611963272f*std::fma(x,y,y*x),negative*y,
    std::fma(.236543670296669f,z*z,-.07884789258241653f),negative*x,
    .1365685611963272f*std::fma(x,x,-(y*y))};
}
}
// Output is EM1's camera-space irradiance coefficients, NOT yet the native
// EM2 material constant layout. Visibility must be supplied for raycast lights.
// Host double pow/cos bit parity with the retail libraries is not established.
inline LightCoefficients EvaluateAuthoredLighting(std::span<const AuthoredRuntimeLight> lights,
    LightVector position,LightRotation camera,std::span<const float> visibility={}) {
  using Kind=AuthoredRuntimeLight::Kind;
  for(float f:position)nif::Check(std::isfinite(f),"Nonfinite SH sample position");
  for(auto row:camera)for(float f:row)nif::Check(std::isfinite(f),"Nonfinite SH camera rotation");
  nif::Check(visibility.empty() || visibility.size()==lights.size(),"SH visibility count mismatch");
  LightCoefficients result{};
  for(size_t i=0;i<lights.size();++i) {
    const auto& light=lights[i];
    for(auto v:{light.position,light.direction,light.ambient,light.diffuse,light.attenuation})for(float f:v)nif::Check(std::isfinite(f),"Nonfinite SH light");
    for(float f:{light.dimmer,light.range_start,light.range_end,light.inner_degrees,light.outer_degrees,light.spot_exponent})nif::Check(std::isfinite(f),"Nonfinite SH light scalar");
    nif::Check(!light.raycast || !visibility.empty(),"SH raycast visibility is required");
    const float visible=visibility.empty()?1:visibility[i];
    nif::Check(std::isfinite(visible) && visible>=0 && visible<=1,"Invalid SH visibility");
    if(light.dimmer==0)continue;
    LightVector delta{};for(unsigned j=0;j<3;++j)delta[j]=light.position[j]-position[j];
    auto [distance,direction]=lighting_detail::Distance(delta);
    if(distance==0 && light.kind!=Kind::Directional)continue;
    float attenuation=1;
    if(light.range_end>0) {
      if(distance>light.range_end)continue;
      if(distance>=light.range_start)attenuation=1-(distance-light.range_start)/(light.range_end-light.range_start);
    } else if(light.kind==Kind::Point || light.kind==Kind::Spot) {
      const auto [constant,linear,quadratic]=light.attenuation;
      const float denominator=constant+std::fma(distance,quadratic*distance,linear*distance);
      if(double(denominator)<1e-6)continue;
      attenuation=std::clamp(1/denominator,0.f,1.f);
    }
    float cone=1;
    if(light.kind==Kind::Spot) {
      const float cosine=std::fma(light.direction[0],-direction[0],light.direction[1]*-direction[1])+light.direction[2]*-direction[2];
      const float inner=lighting_detail::Cosine(light.inner_degrees),outer=lighting_detail::Cosine(light.outer_degrees);
      if(cosine<=inner) {
        if(cosine<outer)continue;
        cone=float(std::pow(double((cosine-outer)/(inner-outer)),double(light.spot_exponent)));
      }
    }
    float weight=cone*(light.dimmer*attenuation);nif::Check(std::isfinite(weight),"Invalid SH attenuation interval");
    if(weight<.003921568859368563f)continue;
    weight*=visible;const float sign=light.subtractive?-1.f:1.f;
    const float ambient_scale=3.544907808303833f*weight;
    for(unsigned j=0;j<3;++j)result[0][j]=std::fma((light.ambient[j]*255.f)*sign,ambient_scale,result[0][j]);
    if(light.kind==Kind::Ambient)continue;
    if(light.kind==Kind::Directional)for(unsigned j=0;j<3;++j)direction[j]=-light.direction[j];
    LightVector rotated{};for(unsigned j=0;j<3;++j)rotated[j]=std::fma(camera[j][2],direction[2],std::fma(camera[j][0],direction[0],camera[j][1]*direction[1]));
    const auto basis=lighting_detail::Project({-rotated[0],rotated[1],-rotated[2]});const float scale=2.9567930698394775f*weight;
    for(unsigned b=0;b<9;++b)for(unsigned j=0;j<3;++j)result[b][j]=std::fma(((light.diffuse[j]*255.f)*sign)*scale,basis[b],result[b][j]);
  }
  for(auto row:result)for(float f:row)nif::Check(std::isfinite(f),"SH coefficient overflow");
  return result;
}
}
