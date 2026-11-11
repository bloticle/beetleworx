
  Texture2D<float4> ui:register(t0);cbuffer Settings:register(b0){float white;};
  float4 vs(uint id:SV_VertexID):SV_Position{float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);}
  float decode(float v){return v<=.04045?v/12.92:pow((v+.055)/1.055,2.4);}
  float4 ps(float4 p:SV_Position):SV_Target{float4 v=ui.Load(int3(uint2(p.xy),0));float a=saturate(v.a);if(a==0)return 0;float3 c=saturate(v.rgb/a);return float4(float3(decode(c.r),decode(c.g),decode(c.b))*(white/80)*a,a);}