// RenoDX's psychotm_test30, unchanged (prepare30.py: test30.hlsl and a shim of its helpers),
// with every parameter bbport passes (glob.h).
#include "tonemap/psychov/test30.hlsl"
StructuredBuffer<float4> inputs : register(t0);
RWStructuredBuffer<float4> outputs : register(u1);
struct P {
  float peak; int mode; int boundary; float compression;
  float exposure; float highlights; float shadows; float contrast;
  float purity; float cone; float adaptive; float background;
  float gamut_compression; float pad0; float pad1; float pad2;
};
[[vk::push_constant]] P pc;
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  outputs[id.x] = float4(psychotm_test30(
      inputs[id.x].xyz, pc.peak, pc.exposure, pc.highlights, pc.shadows, pc.contrast, pc.purity,
      1.f, 100.f, 1.f, 1.f, 0, pc.cone, pc.adaptive.xxx, pc.background.xxx, pc.gamut_compression,
      pc.mode, 1.f, pc.compression, pc.boundary), 1.f);
}
