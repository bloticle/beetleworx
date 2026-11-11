Texture2DArray<float4> depthImage : register(t0);
RWTexture2D<float4> positions : register(u0);
RWTexture2D<float4> normals : register(u1);
cbuffer Geometry : register(b0) {
    float4 inverseRows[4];
    float4 clipFromUV; // scale XY, bias XY, explicitly supplied by caller
    float4 cameraAndDiscontinuity; // world XYZ and relative view-depth jump limit
    float4 viewDirection;
    uint2 dimensions;
};
bool PositionAt(int2 pixel, out float3 world) {
    world=0;
    if(any(pixel<0) || any(pixel>=int2(dimensions)))return false;
    float2 uv=(float2(pixel)+.5)/float2(dimensions);
    return SceneUnproject(uv*clipFromUV.xy+clipFromUV.zw,
                          depthImage.Load(int4(pixel,0,0)).r,inverseRows,world);
}
bool Neighbour(int2 pixel,float3 center,float distanceToCamera,out float3 world) {
    if(!PositionAt(pixel,world))return false;
    return abs(dot(world-center,viewDirection.xyz))
         <=cameraAndDiscontinuity.w*max(distanceToCamera,1e-5);
}
float NormalHalf(float value) {
    // Store an already representable nearest-even half. Some typed UAV stores
    // truncate, which otherwise biases even axis-aligned unit normals.
    uint bits=asuint(clamp(value,-1,1)),sign=(bits>>16)&0x8000,magnitude=bits&0x7fffffff;
    uint halfBits;
    if(magnitude>=0x38800000)halfBits=(magnitude-0x38000000+0xfff+((magnitude>>13)&1))>>13;
    else {uint exponent=magnitude>>23;if(exponent<102)halfBits=0;
        else {uint mantissa=(magnitude&0x7fffff)|0x800000,shift=126-exponent;
              halfBits=(mantissa+((1u<<(shift-1))-1)+((mantissa>>shift)&1))>>shift;}}
    return f16tof32(sign|halfBits);
}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=dimensions))return;
    int2 pixel=int2(id.xy);float3 center;
    if(!PositionAt(pixel,center)){positions[pixel]=0;normals[pixel]=0;return;}
    positions[pixel]=float4(center,1);
    float distanceToCamera=abs(dot(center-cameraAndDiscontinuity.xyz,viewDirection.xyz));
    float3 l,r,u,d;
    bool vl=Neighbour(pixel+int2(-1,0),center,distanceToCamera,l);
    bool vr=Neighbour(pixel+int2(1,0),center,distanceToCamera,r);
    bool vu=Neighbour(pixel+int2(0,-1),center,distanceToCamera,u);
    bool vd=Neighbour(pixel+int2(0,1),center,distanceToCamera,d);
    if(!(vl||vr) || !(vu||vd)){normals[pixel]=0;return;}
    // Choose the closest valid surface on each axis rather than crossing a
    // silhouette to build a spurious normal between foreground/background.
    float3 dx=vl&&(!vr||dot(center-l,center-l)<dot(r-center,r-center))?center-l:r-center;
    float3 dy=vu&&(!vd||dot(center-u,center-u)<dot(d-center,d-center))?center-u:d-center;
    float3 n=cross(dx,dy);float magnitude=dot(n,n);
    float scale=dot(dx,dx)*dot(dy,dy);
    if(!isfinite(magnitude)||!isfinite(scale)||scale<=0||magnitude<=scale*1e-8){normals[pixel]=0;return;}
    n*=rsqrt(magnitude);
    if(dot(n,cameraAndDiscontinuity.xyz-center)<0)n=-n;
    normals[pixel]=float4(NormalHalf(n.x),NormalHalf(n.y),NormalHalf(n.z),1);
}
