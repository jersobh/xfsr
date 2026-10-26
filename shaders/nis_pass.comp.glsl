#version 450
// xfsr NIS-style spatial upscaler — single-pass edge-directional scale + optional sharpen.
// Inspired by NVIDIA Image Scaling (spatial only; not a full NVSDK port).
// Workgroup 16×16 → one display pixel per invocation.

layout(local_size_x = 16, local_size_y = 16) in;

layout(binding = 0) uniform NisParams {
    vec4 sizes;     // renderW, renderH, displayW, displayH
    vec4 opts;      // sharpness [0..1], swizzleBgra, pad, pad
} u;

layout(binding = 1) uniform texture2D srcTex;
layout(binding = 2, rgba8) writeonly uniform image2D dstImg;
layout(binding = 3) uniform sampler srcSamp;

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 sampleLin(vec2 uv) {
    return textureLod(sampler2D(srcTex, srcSamp), uv, 0.0).rgb;
}

// Catmull-Rom weight for one axis (cubic).
float crWeight(float x) {
    x = abs(x);
    if (x <= 1.0) return 1.5 * x * x * x - 2.5 * x * x + 1.0;
    if (x <= 2.0) return -0.5 * x * x * x + 2.5 * x * x - 4.0 * x + 2.0;
    return 0.0;
}

vec3 bicubic(vec2 uv, vec2 texSize) {
    vec2 px = uv * texSize - 0.5;
    vec2 i = floor(px);
    vec2 f = px - i;
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int y = -1; y <= 2; ++y) {
        float wy = crWeight(f.y - float(y));
        for (int x = -1; x <= 2; ++x) {
            float wx = crWeight(f.x - float(x));
            float w = wx * wy;
            vec2 sp = (i + vec2(float(x), float(y)) + 0.5) / texSize;
            acc += sampleLin(sp) * w;
            wsum += w;
        }
    }
    return acc / max(wsum, 1e-5);
}

// 5-tap directional edge filter (NIS-like): blend bicubic toward the edge axis.
vec3 edgeScale(vec2 uv, vec2 texSize) {
    vec2 texel = 1.0 / texSize;
    float lC = luma(sampleLin(uv));
    float lL = luma(sampleLin(uv + vec2(-texel.x, 0.0)));
    float lR = luma(sampleLin(uv + vec2( texel.x, 0.0)));
    float lD = luma(sampleLin(uv + vec2(0.0, -texel.y)));
    float lU = luma(sampleLin(uv + vec2(0.0,  texel.y)));
    float gx = lR - lL;
    float gy = lU - lD;
    float g = sqrt(gx * gx + gy * gy);
    vec3 soft = bicubic(uv, texSize);
    // Along-edge: average of two neighbors on the weaker gradient axis.
    vec3 along = (abs(gx) > abs(gy))
        ? 0.5 * (sampleLin(uv + vec2(0.0, -texel.y)) + sampleLin(uv + vec2(0.0, texel.y)))
        : 0.5 * (sampleLin(uv + vec2(-texel.x, 0.0)) + sampleLin(uv + vec2(texel.x, 0.0)));
    float edge = clamp(g * 4.0, 0.0, 1.0);
    return mix(soft, mix(soft, along, 0.35), edge);
}

// Lightweight contrast-adaptive sharpen (CAS-ish).
vec3 sharpen(vec3 c, vec2 uv, vec2 texSize, float sharp) {
    if (sharp <= 0.001) return c;
    vec2 texel = 1.0 / texSize;
    vec3 n = sampleLin(uv + vec2(0.0, -texel.y));
    vec3 s = sampleLin(uv + vec2(0.0,  texel.y));
    vec3 e = sampleLin(uv + vec2( texel.x, 0.0));
    vec3 w = sampleLin(uv + vec2(-texel.x, 0.0));
    vec3 mn = min(c, min(min(n, s), min(e, w)));
    vec3 mx = max(c, max(max(n, s), max(e, w)));
    vec3 amp = clamp(min(mn, 1.0 - mx) / max(mx - mn, vec3(1e-4)), 0.0, 1.0);
    float wts = -0.125 - 0.075 * sharp; // stronger with sharpness
    vec3 filt = (n + s + e + w) * wts + c;
    float inv = 1.0 / (1.0 + 4.0 * wts);
    vec3 outc = filt * inv;
    return mix(c, outc, amp * sharp);
}

void main() {
    ivec2 dst = ivec2(gl_GlobalInvocationID.xy);
    int dw = int(u.sizes.z + 0.5);
    int dh = int(u.sizes.w + 0.5);
    if (dst.x >= dw || dst.y >= dh) return;

    vec2 displaySize = vec2(u.sizes.z, u.sizes.w);
    vec2 renderSize = max(vec2(u.sizes.x, u.sizes.y), vec2(1.0));
    vec2 uv = (vec2(dst) + 0.5) / displaySize;

    vec3 rgb = edgeScale(uv, renderSize);
    // Sharpen in render UV space (sample neighbors at source res).
    rgb = sharpen(rgb, uv, renderSize, clamp(u.opts.x, 0.0, 1.0));
    rgb = clamp(rgb, 0.0, 1.0);

    vec4 outc = vec4(rgb, 1.0);
    if (u.opts.y > 0.5) outc = vec4(outc.bgr, outc.a);
    imageStore(dstImg, dst, outc);
}
