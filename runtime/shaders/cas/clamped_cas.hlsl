// CAS presenter integration adapted from Xenia (Copyright 2022 Ben Vanik).
// BSD license: LICENSE.xenia. AMD headers retain their MIT notices.
// Source pins and hashes: upstream.json.
#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

cbuffer Settings : register(b0) {
    int2 output_offset : packoffset(c0.x);
#if CAS_RESAMPLE
    float2 input_output_ratio : packoffset(c0.z);
    float sharpness : packoffset(c1.x);
#else
    float sharpness : packoffset(c0.z);
#endif
#if CAS_HDR
    float hdr_white : packoffset(c8.x);
    float hdr_peak : packoffset(c8.y);
    uint hdr_flags : packoffset(c8.z);
#endif
};
Texture2D<float4> source_texture : register(t0);
#if CAS_HDR
#include "hdr_cas_colour.hlsli"
#endif

float3 CasLoad(int2 p) {
    uint width, height;
    source_texture.GetDimensions(width, height);
    // Integer texture loads do not use a sampler's clamp addressing mode.
    // Every CAS tap, including resampling taps, must stay inside the source.
    p = clamp(p, int2(0, 0), int2(width - 1, height - 1));
    float3 color=source_texture.Load(int3(p, 0)).rgb;
#if CAS_HDR
    color=HdrCasRead(color);
#endif
    return color;
}
void CasInput(inout float r, inout float g, inout float b) {
#if !CAS_HDR
    r *= r; g *= g; b *= b;
#endif
}
#if CAS_HDR
// The fast reciprocal approximation measurably changes constant HDR radiance
// after resampling. Use the library's accurate arithmetic path for HDR.
#define CAS_GO_SLOWER 1
#endif
#include "ffx_cas.h"

#if CAS_DITHER
#include "noise.xesli"
static const float dither_noise[256] = {
    XeBlueNoise16x16Values0Until256(1.0f / 256.0f / 255.0f,
                                 (-0.5f + 0.5f / 256.0f) / 255.0f)
};
#endif

float4 main(float4 position : SV_Position) : SV_Target0 {
    uint2 pixel = uint2(int2(position.xy) - output_offset);
#if CAS_RESAMPLE
    uint4 const0 = uint4(asuint(input_output_ratio),
                         asuint(0.5f * input_output_ratio - 0.5f));
#else
    uint4 const0 = asuint(float4(1.0f, 1.0f, 0.0f, 0.0f));
#endif
    uint4 const1 = uint4(asuint(sharpness), f32tof16(sharpness),
                         asuint(8.0f), 0u);
    float3 color;
    CasFilter(color.r, color.g, color.b, pixel, const0, const1,
              CAS_RESAMPLE == 0);
#if CAS_HDR
    color=HdrCasWrite(color);
#else
    color = sqrt(color);
#endif
#if CAS_DITHER && !CAS_HDR
    uint2 noise_pixel = pixel & 15u;
    color = saturate(color + dither_noise[noise_pixel.y * 16u + noise_pixel.x]);
#endif
    return float4(color, 1.0f);
}
