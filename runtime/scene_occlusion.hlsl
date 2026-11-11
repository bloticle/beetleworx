Texture2D<float4> positions : register(t0);
Texture2D<float4> normals : register(t1);
RWTexture2D<float4> result : register(u0);
cbuffer Settings : register(b0) {
    float worldRadius,pixelRadius,bias,strength;
    uint2 dimensions,outputDimensions;
};
float HalfUnit(float value) {
    uint bits=asuint(clamp(value,-1,1)),sign=(bits>>16)&0x8000,m=bits&0x7fffffff,h;
    if(m>=0x38800000)h=(m-0x38000000+0xfff+((m>>13)&1))>>13;
    else {uint e=m>>23;if(e<102)h=0;else {uint v=(m&0x7fffff)|0x800000,s=126-e;h=(v+((1u<<(s-1))-1)+((v>>s)&1))>>s;}}
    return f16tof32(sign|h);
}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=outputDimensions))return;
    int2 pixel=min(int2(id.xy*2+1),int2(dimensions)-1);
    float4 p=positions.Load(int3(pixel,0)),n=normals.Load(int3(pixel,0));
    if(p.w!=1 || n.w!=1 || !all(isfinite(p)) || !all(isfinite(n)) || dot(n.xyz,n.xyz)<.5){result[id.xy]=float4(0,0,0,1);return;}
    float3 normal=normalize(n.xyz),bend=0;float occlusion=0;
    static const float2 directions[8]={float2(1,0),float2(.70710678,.70710678),float2(0,1),float2(-.70710678,.70710678),float2(-1,0),float2(-.70710678,-.70710678),float2(0,-1),float2(.70710678,-.70710678)};
    if(strength>0)for(uint direction=0;direction<8;++direction){
        float horizon=0;float3 blockedTangent=0;
        for(uint step=1;step<=4;++step){
            int2 samplePixel=pixel+int2(round(directions[direction]*max(1,pixelRadius*step*.25)));
            if(any(samplePixel<0)||any(samplePixel>=int2(dimensions)))continue;
            float4 q=positions.Load(int3(samplePixel,0));if(q.w!=1 || !all(isfinite(q)))continue;
            float3 delta=q.xyz-p.xyz;float d2=dot(delta,delta);
            if(d2<1e-12 || d2>=worldRadius*worldRadius)continue;
            float distance=sqrt(d2),height=dot(normal,delta);
            float h=saturate((height-bias)/distance)*(1-d2/(worldRadius*worldRadius));
            if(h>horizon){horizon=h;float3 tangent=delta-normal*height;float t2=dot(tangent,tangent);blockedTangent=t2>1e-12?tangent*rsqrt(t2):0;}
        }
        occlusion+=horizon*horizon;bend+=blockedTangent*horizon;
    }
    float visibility=1-saturate(occlusion*(strength/8));
    float3 bent=normalize(normal-bend*(strength/16));
    result[id.xy]=float4(HalfUnit(bent.x),HalfUnit(bent.y),HalfUnit(bent.z),HalfUnit(visibility));
}
