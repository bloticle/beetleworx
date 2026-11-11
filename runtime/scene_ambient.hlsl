Texture2D<float4> sceneColour : register(t0);
Texture2D<float4> materialAmbient : register(t1);
Texture2D<float4> positions : register(t2);
Texture2D<float4> normals : register(t3);
Texture2D<float4> occlusion : register(t4);
Texture2D<float4> indirectLight : register(t5);
Texture2D<float4> surfaceData : register(t6);
RWTexture2D<float4> result : register(u0);
cbuffer Settings : register(b0) {
    uint2 dimensions,aoDimensions;
    float strength,planeDistance,normalCosine,indirectStrength;
    float materialDistance;float3 padding;
};
bool Geometry(float4 p,float4 n) {
    return p.w==1 && n.w==1 && all(isfinite(p)) && all(isfinite(n)) && dot(n.xyz,n.xyz)>.5;
}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=dimensions))return;
    float4 colour=sceneColour.Load(int3(id.xy,0));
    float3 ambient=materialAmbient.Load(int3(id.xy,0)).rgb;
    if(!all(isfinite(colour)) || !all(isfinite(ambient)) || ((strength==0 || all(ambient==0)) && indirectStrength==0)){result[id.xy]=colour;return;}
    float4 p=positions.Load(int3(id.xy,0)),n=normals.Load(int3(id.xy,0));
    if(!Geometry(p,n)){result[id.xy]=colour;return;}
    float3 normal=normalize(n.xyz);float2 coordinate=(float2(id.xy)-1)*.5;
    int2 base=int2(floor(coordinate));float2 fraction=frac(coordinate);float total=0,visibility=0,indirectTotal=0;float3 indirect=0;
    float4 material=0;if(indirectStrength>0)material=surfaceData.Load(int3(id.xy,0));
    [unroll]for(uint y=0;y<2;++y)[unroll]for(uint x=0;x<2;++x){
        int2 sampleIndex=clamp(base+int2(x,y),int2(0,0),int2(aoDimensions)-1);
        int2 pixel=min(sampleIndex*2+1,int2(dimensions)-1);
        float4 q=positions.Load(int3(pixel,0)),qn=normals.Load(int3(pixel,0)),ao=occlusion.Load(int3(sampleIndex,0));
        if(!Geometry(q,qn) || !all(isfinite(ao)))continue;
        float agreement=dot(normal,normalize(qn.xyz));
        float separation=abs(dot(normal,q.xyz-p.xyz));
        if(agreement<normalCosine || separation>=planeDistance)continue;
        float w=(x?fraction.x:1-fraction.x)*(y?fraction.y:1-fraction.y);
        w*=saturate(1-separation/planeDistance)*saturate((agreement-normalCosine)/(1-normalCosine));
        visibility+=saturate(ao.w)*w;total+=w;
        if(indirectStrength>0 && material.a>=0 && all(isfinite(material))){
            float4 bounce=indirectLight.Load(int3(sampleIndex,0)),other=surfaceData.Load(int3(pixel,0));
            if(bounce.a==1 && other.a>=0 && all(isfinite(bounce)) && all(isfinite(other)) && all(abs(material.rgb-other.rgb)<=materialDistance)){
                indirect+=bounce.rgb*w;indirectTotal+=w;
            }
        }
    }
    float v=total>1e-6?visibility/total:1;
    // Preserve signed HDR values, highlights and original alpha. No final-colour
    // darkening, clamp or gamma conversion; only the captured material term moves
    // toward its independently evaluated SH-zero baseline.
    float3 lit=colour.rgb-ambient*((1-v)*strength);
    if(indirectTotal>1e-6)lit=clamp(lit+indirect/indirectTotal*indirectStrength,-65504,65504);
    result[id.xy]=float4(lit,colour.a);
}
