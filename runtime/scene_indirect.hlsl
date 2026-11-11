// Keep sample order and equations identical while allowing a measured choice
// of loop layout. 0 is the original fully unrolled reference, 1 loops emitters,
// 2 also loops the four visibility probes. No quality/sample-count reduction.
#ifndef EM2_GI_LOOP_LAYOUT
#define EM2_GI_LOOP_LAYOUT 0
#endif
#if EM2_GI_LOOP_LAYOUT
#define EM2_EMITTER_LOOP [loop]
#else
#define EM2_EMITTER_LOOP [unroll]
#endif
#if EM2_GI_LOOP_LAYOUT == 2
#define EM2_VISIBILITY_LOOP [loop]
#else
#define EM2_VISIBILITY_LOOP [unroll]
#endif
Texture2D<float4> sceneColour:register(t0);
Texture2D<float4> surface:register(t1);
Texture2D<float4> positions:register(t2);
Texture2D<float4> normals:register(t3);
RWTexture2D<float4> result:register(u0);
cbuffer Settings:register(b0){uint2 dimensions,outDimensions;float radius,pixelRadius,thickness,strength;};
bool Point(int2 p,out float3 world){
 world=0;if(any(p<0)||any(p>=int2(dimensions)))return false;
 float4 value=positions.Load(int3(p,0));if(value.w!=1||!all(isfinite(value)))return false;world=value.xyz;return true;
}
bool Normal(int2 p,out float3 n){
 float4 value=normals.Load(int3(p,0));n=0;if(value.w!=1||!all(isfinite(value))||dot(value.xyz,value.xyz)<.5)return false;n=normalize(value.xyz);return true;
}
bool Derivative(int2 p,int2 axis,float3 q,float3 n,out float3 delta){
 float3 a,b;bool va=Point(p+axis,a),vb=Point(p-axis,b);a-=q;b=q-b;
 va=va&&abs(dot(a,n))<=thickness&&dot(a,a)<radius*radius*.25;
 vb=vb&&abs(dot(b,n))<=thickness&&dot(b,b)<radius*radius*.25;
 delta=va&&(!vb||dot(a,a)<dot(b,b))?a:b;return va||vb;
}
bool Visible(int2 p,int2 q,float3 a,float3 b,float3 na,float3 nb){
 // Projected straight lines stay straight under perspective. Probe their
 // visible surfels in world space; never interpolate world depth linearly.
 float3 start=a+na*(thickness*.5),end=b+nb*(thickness*.5),ray=end-start;
 float length2=dot(ray,ray);if(length2<1e-10)return false;
 EM2_VISIBILITY_LOOP for(uint step=1;step<=4;++step){
  int2 pixel=int2(round(lerp(float2(p),float2(q),float(step)/5)));float3 blocker;
  if(!Point(pixel,blocker))continue;
  if(dot(blocker-a,blocker-a)<=4*thickness*thickness||dot(blocker-b,blocker-b)<=4*thickness*thickness)continue;
  float t=dot(blocker-start,ray)/length2;
  if(t>0&&t<1&&dot(blocker-(start+ray*t),blocker-(start+ray*t))<thickness*thickness)return false;
 }
 return true;
}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=outDimensions))return;
 int2 p=min(int2(id.xy)*2+1,int2(dimensions)-1);float3 a,na;
 float4 material=surface.Load(int3(p,0));
 if(!Point(p,a)||!Normal(p,na)||material.a<0||!all(isfinite(material))){result[id.xy]=0;return;}
 if(strength==0){result[id.xy]=float4(0,0,0,1);return;}
 float3 sum=0;float total=0;
 EM2_EMITTER_LOOP for(uint y=0;y<4;++y) EM2_EMITTER_LOOP for(uint x=0;x<4;++x){
  float2 offset=(float2(x,y)+.5)*.5-1;
  int2 q=int2(round(float2(p)+offset*pixelRadius));float3 b,nb;
  if(all(q==p)||!Point(q,b)||!Normal(q,nb))continue;
  float3 delta=b-a;float distance2=dot(delta,delta);
  if(!isfinite(distance2)||distance2<=4*thickness*thickness||distance2>=radius*radius)continue;
  float distance=sqrt(distance2);float3 direction=delta/distance;
  float receiver=max(0,dot(na,direction)),emitter=max(0,dot(nb,-direction));
  if(receiver==0||emitter==0)continue;
  float4 radiance=sceneColour.Load(int3(q,0));if(!all(isfinite(radiance)))continue;
  float3 dx,dy;if(!Derivative(q,int2(1,0),b,nb,dx)||!Derivative(q,int2(0,1),b,nb,dy))continue;
  float area=abs(dot(cross(dx,dy),nb));if(!isfinite(area)||area<=1e-12||!Visible(p,q,a,b,na,nb))continue;
  float falloff=1-distance/radius;
  // Four-by-four stratified screen cells, converted to emitter world area.
  float weight=receiver*emitter*area*(pixelRadius*pixelRadius*.25)/(3.141592653589793*distance2)*falloff*falloff;
  if(!isfinite(weight)||weight<=0)continue;
  sum+=max(radiance.rgb,0)*weight;total+=weight;
 }
 // Bound the estimated transport fraction to one. Preserve HDR radiance;
 // only the explicitly linear diffuse reflectance is bounded to [0,1].
 result[id.xy]=float4(saturate(material.rgb)*sum/max(total,1)*strength,1);
}
