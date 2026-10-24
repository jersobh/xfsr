#version 450
#extension GL_GOOGLE_include_directive : enable
#if !SAMPLE_SLOW_FALLBACK
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_shader_16bit_storage : require
#endif

// Based on AMD FidelityFX FSR 1.0 sample (GPUOpen-Effects/FidelityFX-FSR)
// SAMPLE_SLOW_FALLBACK=1 → FP32; =0 → packed FP16 (Vega / RDNA)

layout(set = 0, binding = 0) uniform const_buffer {
    uvec4 Const0;
    uvec4 Const1;
    uvec4 Const2;
    uvec4 Const3;
    uvec4 Sample;
};

#define A_GPU 1
#define A_GLSL 1
#if !SAMPLE_SLOW_FALLBACK
#define A_HALF 1
#endif

#include "ffx_a.h"

layout(set = 0, binding = 1) uniform texture2D InputTexture;
layout(set = 0, binding = 2, rgba8) uniform image2D OutputTexture;
layout(set = 0, binding = 3) uniform sampler InputSampler;

#if SAMPLE_EASU
#if SAMPLE_SLOW_FALLBACK
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(sampler2D(InputTexture, InputSampler), p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(sampler2D(InputTexture, InputSampler), p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(sampler2D(InputTexture, InputSampler), p, 2); }
#else
#define FSR_EASU_H 1
AH4 FsrEasuRH(AF2 p) { return AH4(textureGather(sampler2D(InputTexture, InputSampler), p, 0)); }
AH4 FsrEasuGH(AF2 p) { return AH4(textureGather(sampler2D(InputTexture, InputSampler), p, 1)); }
AH4 FsrEasuBH(AF2 p) { return AH4(textureGather(sampler2D(InputTexture, InputSampler), p, 2)); }
#endif
#endif

#if SAMPLE_RCAS
#if SAMPLE_SLOW_FALLBACK
#define FSR_RCAS_F
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(sampler2D(InputTexture, InputSampler), ASU2(p), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#else
#define FSR_RCAS_H
AH4 FsrRcasLoadH(ASW2 p) { return AH4(texelFetch(sampler2D(InputTexture, InputSampler), ASU2(p), 0)); }
void FsrRcasInputH(inout AH1 r, inout AH1 g, inout AH1 b) {}
#endif
#endif

#include "ffx_fsr1.h"

// Sample.y != 0 → store BGRA (swapchain is B8G8R8A8_*); skip a fullscreen blit/copy.
void StoreOut(AU2 pos, AF3 c) {
    AF4 o = AF4(c, 1.0);
    if (Sample.y != 0u) o = AF4(c.b, c.g, c.r, 1.0);
    imageStore(OutputTexture, ASU2(pos), o);
}

void CurrFilter(AU2 pos) {
#if SAMPLE_EASU
#if SAMPLE_SLOW_FALLBACK
    AF3 c;
    FsrEasuF(c, pos, Const0, Const1, Const2, Const3);
    StoreOut(pos, c);
#else
    AH3 c;
    FsrEasuH(c, pos, Const0, Const1, Const2, Const3);
    StoreOut(pos, AF3(c));
#endif
#endif
#if SAMPLE_RCAS
#if SAMPLE_SLOW_FALLBACK
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, pos, Const0);
    StoreOut(pos, c);
#else
    AH3 c;
    FsrRcasH(c.r, c.g, c.b, pos, Const0);
    StoreOut(pos, AF3(c));
#endif
#endif
}

layout(local_size_x = 64) in;
void main() {
    AU2 gxy = ARmp8x8(gl_LocalInvocationID.x) + AU2(gl_WorkGroupID.x << 4u, gl_WorkGroupID.y << 4u);
    CurrFilter(gxy);
    gxy.x += 8u;
    CurrFilter(gxy);
    gxy.y += 8u;
    CurrFilter(gxy);
    gxy.x -= 8u;
    CurrFilter(gxy);
}
