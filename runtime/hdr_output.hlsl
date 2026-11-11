// Linear scene RGB to linear scRGB. 1 scRGB unit is 80 cd/m^2.
Texture2D<float4> scene : register(t0);
Texture2D<float4> bloom_image : register(t1);
Texture2D<float4> ui_image : register(t2);
RWTexture2D<float4> output_image : register(u0);
cbuffer Settings : register(b0) {
    uint2 extent;
    float exposure;
    float paper_white;
    float peak;
    float bloom_strength;
    uint ui_encoding;
    uint flags;
    uint2 bloom_extent;
    uint2 padding;
};
float finite_channel(float v) {
    return isfinite(v) ? clamp(v,-65504.0,65504.0) : 0.0;
}
float nearest_half(float v) {
    // Make the UAV store exact instead of relying on its float32->float16
    // conversion mode. The tone-mapped range is finite and at most +/-125.
    uint raw=asuint(v),sign=(raw>>16)&0x8000,positive=raw&0x7fffffff;
    uint half_bits;
    if(positive>=0x38800000) {
        half_bits=(positive-0x38000000+0xfff+((positive>>13)&1))>>13;
    } else {
        uint exponent=positive>>23;
        if(exponent<102)half_bits=0;
        else {
            uint mantissa=(positive&0x7fffff)|0x800000;
            uint shift=126-exponent;
            half_bits=(mantissa+((1u<<(shift-1))-1)+((mantissa>>shift)&1))>>shift;
        }
    }
    return f16tof32(sign|half_bits);
}
float3 finite_rgb(float3 v){return float3(finite_channel(v.r),finite_channel(v.g),finite_channel(v.b));}
float3 bloom_at(int2 p){return finite_rgb(bloom_image.Load(int3(clamp(p,int2(0,0),int2(bloom_extent)-1),0)).rgb);}
float3 bloom_sample(uint2 p){
 float2 position=(float2(p)+.5)*float2(bloom_extent)/float2(extent)-.5;
 int2 base=int2(floor(position));float2 f=position-float2(base);
 return lerp(lerp(bloom_at(base),bloom_at(base+int2(1,0)),f.x),lerp(bloom_at(base+int2(0,1)),bloom_at(base+int2(1,1)),f.x),f.y);
}
float decode_srgb(float v){return v<=.04045?v/12.92:pow((v+.055)/1.055,2.4);}
[numthreads(8,8,1)]
void main(uint3 pixel : SV_DispatchThreadID) {
    if(any(pixel.xy >= extent))return;
    float3 input=scene.Load(int3(pixel.xy,0)).rgb;
    input=finite_rgb(input);
    if((flags&1) && bloom_strength>0)input+=bloom_sample(pixel.xy)*bloom_strength;
    float3 nits=input * exposure * paper_white;
    float magnitude=max(abs(nits.r),max(abs(nits.g),abs(nits.b)));
    if(magnitude > paper_white) {
        float room=peak-paper_white;
        float delta=magnitude-paper_white;
        // Continuous value and slope at diffuse white, asymptotic display peak.
        // Division first avoids a large intermediate product for bright inputs.
        float mapped=paper_white + room*(delta/(room+delta));
        nits *= mapped/magnitude;
    }
    float3 scRGB=nits/80.0;
    if(ui_encoding!=0){
        float4 ui=ui_image.Load(int3(pixel.xy,0));float alpha=isfinite(ui.a)?saturate(ui.a):0;
        if(alpha>0){
            float3 color=finite_rgb(ui.rgb);
            if(ui_encoding==2 || ui_encoding==4)color/=alpha;
            color=saturate(color);
            if(ui_encoding>=3)color=float3(decode_srgb(color.r),decode_srgb(color.g),decode_srgb(color.b));
            scRGB=lerp(scRGB,color*(paper_white/80.0),alpha);
        }
    }
    output_image[pixel.xy]=float4(nearest_half(scRGB.r),nearest_half(scRGB.g),nearest_half(scRGB.b),1.0);
}
