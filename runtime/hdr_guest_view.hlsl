// Xbox PWL branch thresholds/scales mirror the native shader translator.
// LDR uses native truncation. Positive HDR extends beyond white instead of
// clamping back to UNORM8. Unsigned guest reads clamp negative RGB to zero.
cbuffer Constants:register(b0){uint width,height,transfer;int exponent_bias;};
Texture2D<float4> scene:register(t0);
RWTexture2D<float4> guest:register(u0);
float encode_pwl(float x){
    float a=max(x,0),scale,offset;
    if(a>=128.0/1023.0){if(a>=512.0/1023.0){scale=1023.0/8.0;offset=128.0/255.0;}else{scale=1023.0/4.0;offset=64.0/255.0;}}
    else if(a>=64.0/1023.0){scale=1023.0/2.0;offset=32.0/255.0;}else{scale=1023.0;offset=0;}
    float value=trunc(a*scale)*(1.0/255.0)+offset;return value;
}
float encode_srgb(float x){float a=max(x,0);float value=a<=.0031308?12.92*a:1.055*pow(a,1.0/2.4)-.055;return value;}
[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){
    if(p.x>=width || p.y>=height)return;float4 value=scene.Load(int3(p.xy,0));
    if(transfer==0){[unroll]for(uint c=0;c<4;++c)value[c]=isfinite(value[c])?floor(saturate(value[c])*255.0+.5)*(1.0/255.0):0;guest[p.xy]=value;return;}
    float scale=exp2(float(exponent_bias));
    [unroll]for(uint c=0;c<3;++c){float x=isfinite(value[c])?value[c]/scale:0;value[c]=(transfer==1?encode_pwl(x):encode_srgb(x))*scale;}
    value.a=saturate(value.a); // Coverage keeps unsigned guest semantics.
    guest[p.xy]=value;
}
