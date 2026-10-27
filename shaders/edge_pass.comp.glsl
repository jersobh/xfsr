#version 450
// xfsr "edge" — spatial upscaler (quality + perf).
// Quality: shared-mem source tile, Di Zenzo RGB structure tensor (box-smoothed),
//          anisotropy-weighted edge-stop cubic + chroma bilinear,
//          ETF along-edge mix, fused display-space CAS (single pass),
//          linear anti-ring, padded CAS (no 16×16 tile seams).
// Perf: Di Zenzo per-pixel + directional + light display-space sharpen.
// Workgroup 16×16 → one display pixel per lane.

layout(local_size_x = 16, local_size_y = 16) in;

layout(binding = 0) uniform EdgeParams {
    vec4 sizes; // renderW, renderH, displayW, displayH
    vec4 opts;  // sharpness, swizzleBgra, mode(0=quality 1=perf), scale
} u;

layout(binding = 1) uniform texture2D srcTex;
layout(binding = 2, rgba8) writeonly uniform image2D dstImg;
layout(binding = 3) uniform sampler srcSamp;

const int SRC_PAD = 3;
const int SRC_DIM = 26; // covers 16 display + UP_PAD halo + cubic taps @ scale≈1
const int UP_PAD = 1;
const int UP_DIM = 16 + 2 * UP_PAD; // 18
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

shared vec3 smSrc[SRC_DIM * SRC_DIM];
shared vec3 smTensor[SRC_DIM * SRC_DIM]; // xyz = (Jxx, Jxy, Jyy) Di Zenzo
shared vec3 smUp[UP_DIM * UP_DIM];

float lumaLin(vec3 c) {
    vec3 x = max(c, vec3(0.0));
    vec3 lin = mix(x / 12.92, pow((x + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), x));
    return dot(lin, LUMA);
}

vec3 toLin(vec3 c) {
    vec3 x = max(c, vec3(0.0));
    return mix(x / 12.92, pow((x + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), x));
}

vec3 toSrgb(vec3 lin) {
    vec3 x = max(lin, vec3(0.0));
    return mix(x * 12.92, 1.055 * pow(x, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), x));
}

vec3 sampleSrc(vec2 uv) {
    return textureLod(sampler2D(srcTex, srcSamp), uv, 0.0).rgb;
}

float mcWeight(float x) {
    x = abs(x);
    const float B = 1.0 / 3.0;
    const float C = 1.0 / 3.0;
    if (x < 1.0) {
        return ((12.0 - 9.0 * B - 6.0 * C) * x * x * x + (-18.0 + 12.0 * B + 6.0 * C) * x * x +
                (6.0 - 2.0 * B)) *
               (1.0 / 6.0);
    }
    if (x < 2.0) {
        return ((-B - 6.0 * C) * x * x * x + (6.0 * B + 30.0 * C) * x * x +
                (-12.0 * B - 48.0 * C) * x + (8.0 * B + 24.0 * C)) *
               (1.0 / 6.0);
    }
    return 0.0;
}

int srcIdx(int x, int y) { return y * SRC_DIM + x; }

vec3 srcAt(ivec2 local) {
    local = clamp(local, ivec2(0), ivec2(SRC_DIM - 1));
    return smSrc[srcIdx(local.x, local.y)];
}

vec3 tensorAt(ivec2 local) {
    local = clamp(local, ivec2(0), ivec2(SRC_DIM - 1));
    return smTensor[srcIdx(local.x, local.y)];
}

// Bilinear from shared source tile (local float coords in source texel space).
vec3 srcBilinear(vec2 local) {
    vec2 i = floor(local);
    vec2 f = local - i;
    ivec2 ii = ivec2(i);
    vec3 a = srcAt(ii);
    vec3 b = srcAt(ii + ivec2(1, 0));
    vec3 c = srcAt(ii + ivec2(0, 1));
    vec3 d = srcAt(ii + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void fillSrcTile(ivec2 groupDispOrigin, vec2 renderSize, vec2 displaySize, out ivec2 tileBase) {
    // Origin includes UP_PAD so halo display pixels still sit in-tile.
    vec2 srcOrig = (vec2(groupDispOrigin) + 0.5) * (renderSize / displaySize) - 0.5;
    tileBase = ivec2(floor(srcOrig)) - ivec2(SRC_PAD);
    ivec2 texSize = max(ivec2(renderSize + 0.5), ivec2(1));

    int lid = int(gl_LocalInvocationIndex);
    int total = SRC_DIM * SRC_DIM;
    for (int i = lid; i < total; i += 256) {
        int sx = i % SRC_DIM;
        int sy = i / SRC_DIM;
        ivec2 p = clamp(tileBase + ivec2(sx, sy), ivec2(0), texSize - 1);
        vec2 uv = (vec2(p) + 0.5) / vec2(texSize);
        smSrc[i] = sampleSrc(uv);
    }
    barrier();
}

// Di Zenzo RGB structure tensor at one shared-tile texel (Sobel per channel, sum outer products).
vec3 diZenzoAt(ivec2 c) {
    vec3 c00 = toLin(srcAt(c + ivec2(-1, -1)));
    vec3 c10 = toLin(srcAt(c + ivec2(0, -1)));
    vec3 c20 = toLin(srcAt(c + ivec2(1, -1)));
    vec3 c01 = toLin(srcAt(c + ivec2(-1, 0)));
    vec3 c21 = toLin(srcAt(c + ivec2(1, 0)));
    vec3 c02 = toLin(srcAt(c + ivec2(-1, 1)));
    vec3 c12 = toLin(srcAt(c + ivec2(0, 1)));
    vec3 c22 = toLin(srcAt(c + ivec2(1, 1)));
    vec3 gx = (c20 + 2.0 * c21 + c22) - (c00 + 2.0 * c01 + c02);
    vec3 gy = (c02 + 2.0 * c12 + c22) - (c00 + 2.0 * c10 + c20);
    return vec3(dot(gx, gx), dot(gx, gy), dot(gy, gy));
}

// 3×3 box-smooth of the structure tensor (smooth J, not squared blurred grads).
void fillAndSmoothTensor() {
    int lid = int(gl_LocalInvocationIndex);
    int total = SRC_DIM * SRC_DIM;
    for (int i = lid; i < total; i += 256) {
        int sx = i % SRC_DIM;
        int sy = i / SRC_DIM;
        ivec2 c = ivec2(sx, sy);
        vec3 acc = vec3(0.0);
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                acc += diZenzoAt(c + ivec2(x, y));
        smTensor[i] = acc * (1.0 / 9.0);
    }
    barrier();
}

// Eigenanalysis of 2×2 structure tensor → edge tangent (ETF) + anisotropy.
void tensorEigen(vec3 J, out vec2 alongDir, out float aniso, out float edgeStr) {
    float Jxx = J.x;
    float Jxy = J.y;
    float Jyy = J.z;
    float trace = Jxx + Jyy;
    float det = Jxx * Jyy - Jxy * Jxy;
    float disc = max(0.25 * trace * trace - det, 0.0);
    float s = sqrt(disc);
    float l1 = 0.5 * trace + s; // major (across edge)
    float l2 = 0.5 * trace - s; // minor (along edge)
    // Minor eigenvector: (Jxx-l2, Jxy) × rotate → along edge; use (-Jxy, Jxx-l1) form.
    vec2 v = vec2(-Jxy, Jxx - l1);
    float vlen = length(v);
    if (vlen < 1e-8) {
        v = vec2(-(Jyy - l1), Jxy);
        vlen = length(v);
    }
    alongDir = (vlen > 1e-8) ? (v / vlen) : vec2(1.0, 0.0);
    float den = max(l1 + l2, 1e-8);
    aniso = clamp((l1 - l2) / den, 0.0, 1.0);
    edgeStr = sqrt(max(l1, 0.0));
}

// Luma-only Mitchell with edge stop; anti-ring clamp in linear; chroma from bilinear soft.
vec3 edgeStopLumaChroma(vec2 srcPxLocal, float edgeSense) {
    vec2 i = floor(srcPxLocal);
    vec2 f = srcPxLocal - i;
    ivec2 ii = ivec2(i);
    float lC = lumaLin(srcAt(ii));

    float yAcc = 0.0;
    float wsum = 0.0;
    float yMn = 1e10;
    float yMx = -1e10;

    for (int y = -1; y <= 2; ++y) {
        float wy = mcWeight(f.y - float(y));
        for (int x = -1; x <= 2; ++x) {
            float wx = mcWeight(f.x - float(x));
            ivec2 p = ii + ivec2(x, y);
            vec3 s = srcAt(p);
            float l = lumaLin(s);
            float stop = exp(-abs(l - lC) * edgeSense);
            float w = wx * wy * max(stop, 0.05);
            yAcc += l * w;
            wsum += w;
            yMn = min(yMn, l);
            yMx = max(yMx, l);
        }
    }
    float ySharp = clamp(yAcc / max(wsum, 1e-5), yMn, yMx);

    vec3 soft = srcBilinear(srcPxLocal);
    vec3 linSoft = toLin(soft);
    float ySoft = max(dot(linSoft, LUMA), 1e-5);
    vec3 linOut = linSoft * (ySharp / ySoft);
    return toSrgb(linOut);
}

vec3 upsampleFromTile(vec2 uv, vec2 renderSize, ivec2 tileBase, float scale) {
    vec2 srcPx = uv * renderSize - 0.5;
    vec2 local = srcPx - vec2(tileBase);
    float edgeSense = mix(6.0, 14.0, clamp((scale - 1.0) / 1.5, 0.0, 1.0));
    vec3 soft = edgeStopLumaChroma(local, edgeSense);

    vec3 J = tensorAt(ivec2(floor(local)));
    vec2 alongDir;
    float aniso, edgeStr;
    tensorEigen(J, alongDir, aniso, edgeStr);

    // Stretch kernel along ETF; compress across (elliptical deformation proxy).
    float stretch = mix(1.0, 1.414, aniso);
    vec3 a0 = srcBilinear(local + alongDir * stretch);
    vec3 a1 = srcBilinear(local - alongDir * stretch);
    vec3 along = 0.5 * (a0 + a1);
    float edgeAmt =
        clamp(edgeStr * mix(2.0, 4.5, clamp(scale / 2.5, 0.0, 1.0)), 0.0, 1.0) * aniso;
    return mix(soft, mix(soft, along, 0.45), edgeAmt);
}

// Perf path: Di Zenzo from texture taps (no shared tensor).
vec3 upsampleLite(vec2 uv, vec2 renderSize, float scale) {
    vec2 texel = 1.0 / renderSize;
    vec3 soft = sampleSrc(uv);

    vec3 c00 = toLin(sampleSrc(uv + vec2(-texel.x, -texel.y)));
    vec3 c10 = toLin(sampleSrc(uv + vec2(0.0, -texel.y)));
    vec3 c20 = toLin(sampleSrc(uv + vec2(texel.x, -texel.y)));
    vec3 c01 = toLin(sampleSrc(uv + vec2(-texel.x, 0.0)));
    vec3 c21 = toLin(sampleSrc(uv + vec2(texel.x, 0.0)));
    vec3 c02 = toLin(sampleSrc(uv + vec2(-texel.x, texel.y)));
    vec3 c12 = toLin(sampleSrc(uv + vec2(0.0, texel.y)));
    vec3 c22 = toLin(sampleSrc(uv + vec2(texel.x, texel.y)));
    vec3 gx = (c20 + 2.0 * c21 + c22) - (c00 + 2.0 * c01 + c02);
    vec3 gy = (c02 + 2.0 * c12 + c22) - (c00 + 2.0 * c10 + c20);
    vec3 J = vec3(dot(gx, gx), dot(gx, gy), dot(gy, gy));
    vec2 alongDir;
    float aniso, edgeStr;
    tensorEigen(J, alongDir, aniso, edgeStr);

    vec3 along =
        0.5 * (sampleSrc(uv + alongDir * texel) + sampleSrc(uv - alongDir * texel));
    float edgeAmt =
        clamp(edgeStr * mix(2.5, 5.5, clamp(scale / 2.5, 0.0, 1.0)), 0.0, 1.0) * aniso;
    return mix(soft, along, edgeAmt * 0.45);
}

vec3 upAt(ivec2 p) {
    p = clamp(p, ivec2(0), ivec2(UP_DIM - 1));
    return smUp[p.y * UP_DIM + p.x];
}

// Display-space CAS using padded shared upscaled tile (real cross-tile neighbors).
// Anisotropy from opts unused here — scale-aware strength only (fused single store).
vec3 sharpenPadded(ivec2 upCoord, float sharp, float scale) {
    vec3 base = upAt(upCoord);
    if (sharp <= 0.001) return base;
    sharp *= mix(1.0, 0.65, clamp((scale - 1.2) / 1.3, 0.0, 1.0));

    vec3 cn = upAt(upCoord + ivec2(0, -1));
    vec3 cs = upAt(upCoord + ivec2(0, 1));
    vec3 ce = upAt(upCoord + ivec2(1, 0));
    vec3 cw = upAt(upCoord + ivec2(-1, 0));
    vec3 c = toLin(base);
    cn = toLin(cn);
    cs = toLin(cs);
    ce = toLin(ce);
    cw = toLin(cw);
    vec3 mn = min(c, min(min(cn, cs), min(ce, cw)));
    vec3 mx = max(c, max(max(cn, cs), max(ce, cw)));
    vec3 amp = clamp(min(mn, 1.0 - mx) / max(mx - mn, vec3(1e-4)), 0.0, 1.0);
    float wts = -0.125 - 0.1 * sharp;
    vec3 filt = (cn + cs + ce + cw) * wts + c;
    float inv = 1.0 / (1.0 + 4.0 * wts);
    return toSrgb(mix(c, filt * inv, amp * sharp));
}

vec3 sharpenLite(vec3 c, vec2 uv, vec2 displaySize, float sharp, float scale) {
    if (sharp <= 0.001) return c;
    sharp *= mix(1.0, 0.7, clamp((scale - 1.2) / 1.3, 0.0, 1.0));
    vec2 t = 1.0 / displaySize;
    vec3 n = sampleSrc(uv + vec2(0.0, -t.y));
    vec3 s = sampleSrc(uv + vec2(0.0, t.y));
    vec3 e = sampleSrc(uv + vec2(t.x, 0.0));
    vec3 w = sampleSrc(uv + vec2(-t.x, 0.0));
    vec3 mn = min(c, min(min(n, s), min(e, w)));
    vec3 mx = max(c, max(max(n, s), max(e, w)));
    vec3 amp = clamp(min(mn, 1.0 - mx) / max(mx - mn, vec3(1e-4)), 0.0, 1.0);
    float wts = -0.1 - 0.08 * sharp;
    vec3 filt = (n + s + e + w) * wts + c;
    return mix(c, filt / (1.0 + 4.0 * wts), amp * sharp);
}

void main() {
    ivec2 dst = ivec2(gl_GlobalInvocationID.xy);
    ivec2 lid = ivec2(gl_LocalInvocationID.xy);
    int dw = int(u.sizes.z + 0.5);
    int dh = int(u.sizes.w + 0.5);
    bool onScreen = dst.x < dw && dst.y < dh;

    vec2 displaySize = max(vec2(u.sizes.z, u.sizes.w), vec2(1.0));
    vec2 renderSize = max(vec2(u.sizes.x, u.sizes.y), vec2(1.0));
    float scale = u.opts.w;
    if (scale < 1.01)
        scale = max(displaySize.x / renderSize.x, displaySize.y / renderSize.y);
    float sharp = clamp(u.opts.x, 0.0, 1.0);
    bool perf = u.opts.z > 0.5;

    vec2 uv = (vec2(dst) + 0.5) / displaySize;
    vec3 rgb = vec3(0.0);

    if (perf) {
        if (onScreen) {
            rgb = upsampleLite(uv, renderSize, scale);
            rgb = sharpenLite(rgb, uv, displaySize, sharp, scale);
        }
    } else {
        ivec2 groupOrig = max(dst - lid, ivec2(0));
        ivec2 tileBase;
        fillSrcTile(groupOrig - ivec2(UP_PAD), renderSize, displaySize, tileBase);
        fillAndSmoothTensor();

        {
            ivec2 dispClamped = clamp(dst, ivec2(0), ivec2(max(dw - 1, 0), max(dh - 1, 0)));
            vec2 uvI = (vec2(dispClamped) + 0.5) / displaySize;
            rgb = upsampleFromTile(uvI, renderSize, tileBase, scale);
            smUp[(lid.y + UP_PAD) * UP_DIM + (lid.x + UP_PAD)] = rgb;
        }
        barrier();

        int lane = int(gl_LocalInvocationIndex);
        int total = UP_DIM * UP_DIM;
        for (int i = lane; i < total; i += 256) {
            int ux = i % UP_DIM;
            int uy = i / UP_DIM;
            bool interior = ux >= UP_PAD && ux < UP_PAD + 16 && uy >= UP_PAD && uy < UP_PAD + 16;
            if (interior) continue;
            ivec2 disp = groupOrig + ivec2(ux - UP_PAD, uy - UP_PAD);
            vec2 uvH = (vec2(disp) + 0.5) / displaySize;
            ivec2 dispClamped = clamp(disp, ivec2(0), ivec2(dw - 1, dh - 1));
            if (disp.x != dispClamped.x || disp.y != dispClamped.y)
                uvH = (vec2(dispClamped) + 0.5) / displaySize;
            smUp[i] = upsampleFromTile(uvH, renderSize, tileBase, scale);
        }
        barrier();

        if (onScreen) {
            ivec2 upCoord = lid + ivec2(UP_PAD);
            rgb = sharpenPadded(upCoord, sharp, scale);
        }
    }

    if (!onScreen) return;
    rgb = clamp(rgb, 0.0, 1.0);
    vec4 outc = vec4(rgb, 1.0);
    if (u.opts.y > 0.5) outc = vec4(outc.bgr, outc.a);
    imageStore(dstImg, dst, outc);
}
