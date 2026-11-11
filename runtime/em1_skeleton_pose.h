#pragma once
#include "em1_skin_deform.h"
namespace em2::em1 {
using WiiQsPose=std::array<float,12>;
namespace qs_detail {
using Pair=std::array<float,2>;
inline Pair Mul(Pair a,Pair b){return {a[0]*b[0],a[1]*b[1]};}
inline Pair Add(Pair a,Pair b){return {a[0]+b[0],a[1]+b[1]};}
inline Pair Madd(Pair a,Pair b,Pair c){return {std::fma(a[0],b[0],c[0]),std::fma(a[1],b[1],c[1])};}
inline Pair Msub(Pair a,Pair b,Pair c){return {std::fma(a[0],b[0],-c[0]),std::fma(a[1],b[1],-c[1])};}
inline Pair Splat(float a){return {a,a};}
inline Pair Load(const WiiQsPose& a,size_t i){return {a[i],a[i+1]};}
inline Pair Neg(Pair a){return {-a[0],-a[1]};}
}
// Original paired-single Qs operation shape. Parent scale stays separate;
// composing affine matrices here changes the game's child translation rule.
inline WiiQsPose ComposeWiiQsPose(const WiiQsPose& parent,const WiiQsPose& local){
  using namespace qs_detail;WiiQsPose out{};
  auto f4=Load(local,0);const auto f0=Splat(local[2]);auto f10=Load(local,2);auto f31=Pair{f4[1],f4[0]};
  auto f11=Load(parent,4);const auto f1=Splat(parent[6]),f13=f11,f12=Load(parent,6);
  f11=Mul(f11,f4);const auto scalar=parent[7];const auto f6=Load(parent,0);
  auto f9=Mul(f4,Splat(f13[0]));auto f2=Madd(f12,f10,f11);auto f30=Mul(f4,f1);f31=Msub(f13,f31,f9);f9=Load(parent,2);
  const float dot=f2[0]+f11[1];f30=Msub(f13,f0,f30);const float factor=std::fma(scalar,scalar,-.5f);
  f31={f31[1],f30[0]};f4=Mul(f4,Splat(factor));f2=Mul(f10,Splat(factor));f31=Neg(f31);
  const auto f29=Splat(scalar);auto f3=Madd(f13,Splat(dot),f4);f10={f30[1],f31[1]};f2=Madd(f12,Splat(dot),f2);
  f4=Madd(f10,f29,f3);f3=Madd(f31,f29,f2);f2=Add(f4,f4);f4=Add(f3,f3);
  const auto xy=Add(f6,f2),zw=Add(f9,f4);out[0]=xy[0];out[1]=xy[1];out[2]=zw[0];out[3]=zw[1];
  const auto f8=Splat(parent[6]);f9=Load(local,4);auto q10=Load(parent,4);const auto f7=Splat(local[6]);
  const auto q6=Pair{f9[1],f9[0]};f2=Mul(f9,Splat(q10[0]));const float lw=local[7],pw=parent[7];
  f3=Mul(f9,f8);f2=Msub(q10,q6,f2);const auto q11=Load(local,6);f3=Msub(q10,f7,f3);const auto p6=Load(parent,6);
  f2=Neg(Pair{f2[1],f3[0]});f3={f3[1],f2[1]};f2=Madd(q11,Splat(pw),f2);f3=Madd(f9,Splat(pw),f3);f3=Madd(q10,Splat(lw),f3);
  f4=Mul(q10,f9);out[4]=f3[0];out[5]=f3[1];f2=Madd(p6,Splat(lw),f2);f3=Madd(p6,q11,f4);
  out[6]=f2[0];out[7]=std::fma(pw,lw,-(f3[0]+f4[1]));
  const auto scaleXY=Mul(Load(parent,8),Load(local,8)),scaleZW=Mul(Load(parent,10),Load(local,10));
  out[8]=scaleXY[0];out[9]=scaleXY[1];out[10]=scaleZW[0];out[11]=scaleZW[1];return out;
}
struct WiiRigBone {int32_t parent=-1;WiiQsPose local{};};
inline bool ResolveWiiModelPoses(std::span<const WiiRigBone> bones,std::vector<WiiQsPose>& output,std::string& error){
  try {
    nif::Check(!bones.empty() && bones.size()<=100000,"Invalid skeleton size");std::vector<WiiQsPose> result;result.reserve(bones.size());
    for(size_t i=0;i<bones.size();++i){const auto& bone=bones[i];nif::Check(bone.parent==-1 || (bone.parent>=0 && size_t(bone.parent)<i),"Invalid/unordered skeleton parent");
      for(const auto value:bone.local)nif::Check(std::isfinite(value),"Nonfinite local bone pose");
      auto pose=bone.parent==-1?bone.local:ComposeWiiQsPose(result[size_t(bone.parent)],bone.local);
      for(const auto value:pose)nif::Check(std::isfinite(value),"Bone composition overflow");result.push_back(pose);
    }
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
inline SkinMatrix WiiQsMatrix(const WiiQsPose& pose){
  const auto x=pose[4],y=pose[5],z=pose[6],w=pose[7],sx=pose[8],sy=pose[9],sz=pose[10];
  return {(1.f-2.f*(y*y+z*z))*sx,2.f*(x*y+z*w)*sx,2.f*(x*z-y*w)*sx,0,
    2.f*(x*y-z*w)*sy,(1.f-2.f*(x*x+z*z))*sy,2.f*(y*z+x*w)*sy,0,
    2.f*(x*z+y*w)*sz,2.f*(y*z-x*w)*sz,(1.f-2.f*(x*x+y*y))*sz,0,
    pose[0],pose[1],pose[2],1};
}
inline bool BuildWiiRigSkinPalette(const WiiCookedSkin& skin,std::span<const WiiRigBone> bones,
    std::vector<SkinMatrix>& output,std::string& error){
  std::vector<WiiQsPose> poses;if(!ResolveWiiModelPoses(bones,poses,error))return false;
  std::vector<SkinMatrix> matrices;matrices.reserve(poses.size());for(const auto& pose:poses)matrices.push_back(WiiQsMatrix(pose));
  return BuildWiiSkinPalette(skin,matrices,output,error);
}

}
