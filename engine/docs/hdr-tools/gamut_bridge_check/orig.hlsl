#include "color/gamut.hlsl"
// The input's w is which value to output (main.cpp): 0 the compression scale, 1 the compressed
// color, 2 compressed then decompressed.
StructuredBuffer<float4> inputs : register(t0);
RWStructuredBuffer<float4> outputs : register(u1);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  float3 c = inputs[id.x].xyz;
  int mode = int(inputs[id.x].w);
  float3 adaptive = lms_from_BT709(float3(1.f, 1.f, 1.f));
  float scale = ComputeGamutCompressionScaleBT709AdaptiveD65(c, adaptive, 1.f);
  float3 compressed = GamutCompressBT709AdaptiveD65(c, adaptive, scale);
  float4 o = float4(scale, 0.f, 0.f, 1.f);
  if (mode == 1) o = float4(compressed, 1.f);
  if (mode == 2) o = float4(GamutDecompressBT709AdaptiveD65(compressed, adaptive, scale), 1.f);
  outputs[id.x] = o;
}