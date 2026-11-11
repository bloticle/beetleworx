
cbuffer Parameters:register(b0){uint sx,sy,dx,dy,mask;};
#if MULTISAMPLED
Texture2DMS<float> old_depth:register(t0);
Texture2DMS<uint2> old_stencil:register(t1);
#else
Texture2D<float> old_depth:register(t0);
Texture2D<uint2> old_stencil:register(t1);
#endif
float4 vs(uint id:SV_VertexID):SV_Position {
 float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
int2 source_pixel(float4 position) {
 uint2 pixel=uint2(position.xy);return int2(((pixel*2+1)*uint2(sx,sy))/(uint2(dx,dy)*2));
}
float depth_ps(float4 position:SV_Position,uint sample_index:SV_SampleIndex):SV_Depth {
#if MULTISAMPLED
 return old_depth.Load(source_pixel(position),sample_index);
#else
 return old_depth.Load(int3(source_pixel(position),0));
#endif
}
void stencil_ps(float4 position:SV_Position,uint sample_index:SV_SampleIndex) {
#if MULTISAMPLED
 uint value=old_stencil.Load(source_pixel(position),sample_index).g;
#else
 uint value=old_stencil.Load(int3(source_pixel(position),0)).g;
#endif
 if(!(value&mask))discard;
}