// HDR adaptation of Xenia's FSR 1 presenter integration (Ben Vanik, 2022).
// See LICENSE.xenia, upstream.json and AMD's MIT notices in the headers.
#define A_GPU 1
#define A_HLSL 1
#include "fsr_ffx_a.h"
cbuffer Settings : register(b0) {
#if HDR_FSR_EASU
    float2 size_ratio : packoffset(c0.x);
    float2 inverse_size : packoffset(c0.z);
#else
    int2 output_offset : packoffset(c0.x);
    float sharpness : packoffset(c0.z);
#endif
    float hdr_white : packoffset(c8.x);
    float hdr_peak : packoffset(c8.y);
    uint hdr_flags : packoffset(c8.z);
};
Texture2D<float4> source_texture : register(t0);
SamplerState filtering : register(s0);
#include "hdr_cas_colour.hlsli"
#if HDR_FSR_EASU
#define FSR_EASU_F 1
float4 GatherHdr(float2 p,uint channel) {
    float4 r=source_texture.GatherRed(filtering,p);
    float4 g=source_texture.GatherGreen(filtering,p);
    float4 b=source_texture.GatherBlue(filtering,p);
    float4 result;
    [unroll]for(uint i=0;i<4;++i)result[i]=HdrFsrRead(float3(r[i],g[i],b[i]))[channel];
    return result;
}
float4 FsrEasuRF(float2 p){return GatherHdr(p,0);}
float4 FsrEasuGF(float2 p){return GatherHdr(p,1);}
float4 FsrEasuBF(float2 p){return GatherHdr(p,2);}
#else
#define FSR_RCAS_F 1
// Preserve constant HDR radiance rather than using a bit-level reciprocal
// approximation for the final RCAS normalization.
#define APrxMedRcpF1(x) rcp(x)
float4 FsrRcasLoadF(int2 p){
    uint width,height;source_texture.GetDimensions(width,height);
    p=clamp(p,int2(0,0),int2(width-1,height-1));
    return float4(HdrFsrRead(source_texture.Load(int3(p,0)).rgb),1);
}
void FsrRcasInputF(inout float r,inout float g,inout float b){}
#endif
#include "ffx_fsr1.h"
float4 main(float4 position : SV_Position) : SV_Target0 {
    float3 color;
#if HDR_FSR_EASU
    uint4 c0=uint4(asuint(size_ratio),asuint(.5*size_ratio-.5));
    uint4 c1=asuint(float4(1,1,1,-1)*inverse_size.xyxy);
    uint4 c2=asuint(float4(-1,2,1,2)*inverse_size.xyxy);
    uint4 c3=uint4(0,asuint(4*inverse_size.y),0,0);
    FsrEasuF(color,uint2(position.xy),c0,c1,c2,c3);
#else
    uint2 pixel=uint2(int2(position.xy)-output_offset);
    uint packed=f32tof16(sharpness);
    uint4 constants=uint4(asuint(sharpness),packed|(packed<<16),0,0);
    FsrRcasF(color.r,color.g,color.b,pixel,constants);
#endif
    return float4(HdrFsrWrite(color),1);
}
