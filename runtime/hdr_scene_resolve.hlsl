// Float resolve from native EDRAM owners. No guest UNORM packing/clamping.
cbuffer Parameters:register(b0) {
 uint4 output_data; // width,height,draw scale x,y
 uint4 resolve_data; // adjusted EDRAM base,pitch,offset x,y in guest pixels
 uint4 source_data; // owner base,pitch,guest samples log2,host samples
 uint4 region_data; // output origin x,y,width,height
 uint4 options; // guest sample mask,swap R/B,exponent bias bits,surface attributes
};
#if MSAA
Texture2DMS<float4> source_image:register(t0);
#else
Texture2D<float4> source_image:register(t0);
#endif
RWTexture2D<float4> destination:register(u0);
[numthreads(8,8,1)]void main(uint3 thread:SV_DispatchThreadID) {
 if(any(thread.xy>=region_data.zw))return;
 uint2 p=thread.xy+region_data.xy;
 if(any(p>=output_data.xy))return;
 uint2 scale=output_data.zw;
 uint2 guest=p/scale+resolve_data.zw;
 uint2 sample_axes=uint2(source_data.z>=2?2:1,source_data.z>=1?2:1);
 uint2 grid=guest*sample_axes;
 uint tile=(resolve_data.x+(grid.y/16)*resolve_data.y+grid.x/80-source_data.x)&2047;
 uint2 owner_grid=uint2((tile%source_data.y)*80+grid.x%80,(tile/source_data.y)*16+grid.y%16);
 uint2 owner_pixel=(owner_grid/sample_axes)*scale+p%scale;
 float4 sum=0;uint count=0;bool unavailable=false;
 [unroll]for(uint s=0;s<4;++s)if(options.x&(1u<<s)) {
#if MSAA
  // Guest sample bit0 is vertical, bit1 horizontal. D3D12's native
  // ownership shader uses horizontal bit0 / vertical bit1 for 4x MSAA.
  uint host=source_data.z==1?(source_data.w==2?(s^1):(s?3:0)):((s&1)<<1)|(s>>1);
  float4 sample_value=source_image.Load(owner_pixel,host);
#else
  float4 sample_value=source_image.Load(int3(owner_pixel,0));
#endif
  sum+=sample_value;
  unavailable=unavailable || (options.w && (sample_value.a<0 || !all(isfinite(sample_value))));
  ++count;
 }
 float4 value=sum/count;
 if(options.y)value=value.bgra;
 value*=exp2(float(asint(options.z)));
 destination[p]=unavailable?float4(0,0,0,-1):value;
}
