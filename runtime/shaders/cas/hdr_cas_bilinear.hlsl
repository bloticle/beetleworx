cbuffer Settings : register(b0) {
    int2 output_offset : packoffset(c0.x);
    float2 inverse_size : packoffset(c0.z);
    float hdr_white : packoffset(c8.x);
    float hdr_peak : packoffset(c8.y);
    uint hdr_flags : packoffset(c8.z);
};
Texture2D<float4> source_texture : register(t0);
SamplerState filtering : register(s0);
#include "hdr_cas_colour.hlsli"
float4 main(float4 p : SV_Position) : SV_Target0 {
    float2 uv=(float2(int2(p.xy)-output_offset)+.5)*inverse_size;
    float3 c=source_texture.SampleLevel(filtering,uv,0).rgb;
    if(hdr_flags&8)return float4(c,1);
    if(hdr_flags&16)return float4(HdrFsrWrite(HdrFsrRead(c)),1);
    return float4(HdrCasWrite(HdrCasRead(c)),1);
}
