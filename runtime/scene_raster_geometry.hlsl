Texture2DArray<float4> depthImage:register(t0,space1);
SamplerState depthSampler:register(s0,space1);
cbuffer Geometry:register(b0,space1) {
    float4 inverseRows[4];
    float4 forwardAndDiscontinuity;
};
struct Input {float4 t[16]:TEXCOORD0;float4 position:SV_Position;};
struct Output {float4 position:SV_Target0;float4 normal:SV_Target1;};
float RasterHalf(float value) {
    uint bits=asuint(clamp(value,-1,1)),sign=(bits>>16)&0x8000,m=bits&0x7fffffff,h;
    if(m>=0x38800000)h=(m-0x38000000+0xfff+((m>>13)&1))>>13;
    else {uint e=m>>23;if(e<102)h=0;else {uint v=(m&0x7fffff)|0x800000,s=126-e;h=(v+((1u<<(s-1))-1)+((v>>s)&1))>>s;}}
    return f16tof32(sign|h);
}
Output main(Input input) {
    float3 world;
    bool valid=SceneUnproject(input.t[0].xy,depthImage.Sample(depthSampler,float3(input.t[1].xy,0)).r,inverseRows,world);
    // Derivatives must execute for the full quad, before any validity branch.
    float3 dx=ddx_fine(world),dy=ddy_fine(world);
    float validDx=ddx_fine(valid?1.0:0.0),validDy=ddy_fine(valid?1.0:0.0);
    Output output;output.position=valid?float4(world,1):0;output.normal=0;
    float3 normal=cross(dx,dy);float magnitude=dot(normal,normal),scale=dot(dx,dx)*dot(dy,dy);
    float distance=abs(dot(world,forwardAndDiscontinuity.xyz));
    bool edge=max(abs(dot(dx,forwardAndDiscontinuity.xyz)),abs(dot(dy,forwardAndDiscontinuity.xyz)))>forwardAndDiscontinuity.w*max(distance,1e-5);
    if(valid && validDx==0 && validDy==0 && !edge && isfinite(magnitude) && isfinite(scale) && scale>0 && magnitude>scale*1e-8){
        normal*=rsqrt(magnitude);if(dot(normal,-world)<0)normal=-normal;
        output.normal=float4(RasterHalf(normal.x),RasterHalf(normal.y),RasterHalf(normal.z),1);
    }
    return output;
}
