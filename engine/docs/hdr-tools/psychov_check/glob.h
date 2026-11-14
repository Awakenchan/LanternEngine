#pragma once
// The kernels' push constants and buffers, shared with the SPIRV-Cross C++ output.
struct PCData {
    float peak; int mode; int boundary; float compression;
    float exposure, highlights, shadows, contrast, purity, cone, adaptive, background;
    float gamut_compression, pad0, pad1, pad2;
};
extern glm::vec4* g_in;
extern glm::vec4* g_out;
extern PCData g_pc;
