#version 450
// xfsr "edge" — spatial upscaler (quality + perf).
// Quality: shared-mem source tile, luma edge-stop cubic + chroma bilinear,
//          structure-tensor along-edge mix, linear anti-ring,
//          padded display-space CAS (no 16×16 tile seams).
// Perf: bilinear + directional + light display-space sharpen.
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
    // Replace luma, keep soft chroma ratios.
    vec3 linOut = linSoft * (ySharp / ySoft);
    return toSrgb(linOut);
}

vec3 upsampleFromTile(vec2 uv, vec2 renderSize, ivec2 tileBase, float scale) {
    vec2 srcPx = uv * renderSize - 0.5;
    vec2 local = srcPx - vec2(tileBase);
    float edgeSense = mix(6.0, 14.0, clamp((scale - 1.0) / 1.5, 0.0, 1.0));
    vec3 soft = edgeStopLumaChroma(local, edgeSense);

    // Structure tensor from 3×3 luma (better diagonals than axis-only).
    float l00 = lumaLin(srcAt(ivec2(floor(local)) + ivec2(-1, -1)));
    float l10 = lumaLin(srcAt(ivec2(floor(local)) + ivec2(0, -1)));
    float l20 = lumaLin(srcAt(ivec2(floor(local)) + ivec2(1, -1)));
    float l01 = lumaLin(srcAt(ivec2(floor(local)) + ivec2(-1, 0)));
    float l21 = lumaLin(srcAt(ivec2(floor(local)) + ivec2(1, 0)));
    float l02 = lumaLin(srcAt(ivec2(floor(local)) + ivec2(-1, 1)));
    float l12 = lumaLin(srcAt(ivec2(floor(local)) + ivec2(0, 1)));
    float l22 = lumaLin(srcAt(ivec2(floor(local)) + ivec2(1, 1)));
    float gx = (l20 + 2.0 * l21 + l22) - (l00 + 2.0 * l01 + l02);
    float gy = (l02 + 2.0 * l12 + l22) - (l00 + 2.0 * l10 + l20);
    float g = sqrt(gx * gx + gy * gy);
    float glen = max(g, 1e-5);
    // Unit vector along the edge (perpendicular to gradient).
    vec2 alongDir = vec2(-gy, gx) / glen;
    vec3 a0 = srcBilinear(local + alongDir);
    vec3 a1 = srcBilinear(local - alongDir);
    vec3 along = 0.5 * (a0 + a1);
    float edgeAmt = clamp(g * mix(2.5, 5.0, clamp(scale / 2.5, 0.0, 1.0)), 0.0, 1.0);
    return mix(soft, mix(soft, along, 0.40), edgeAmt);
}

vec3 upsampleLite(vec2 uv, vec2 renderSize, float scale) {
    vec2 texel = 1.0 / renderSize;
    vec3 soft = sampleSrc(uv);
    float l00 = lumaLin(sampleSrc(uv + vec2(-texel.x, -texel.y)));
    float l10 = lumaLin(sampleSrc(uv + vec2(0.0, -texel.y)));
    float l20 = lumaLin(sampleSrc(uv + vec2(texel.x, -texel.y)));
    float l01 = lumaLin(sampleSrc(uv + vec2(-texel.x, 0.0)));
    float l21 = lumaLin(sampleSrc(uv + vec2(texel.x, 0.0)));
    float l02 = lumaLin(sampleSrc(uv + vec2(-texel.x, texel.y)));
    float l12 = lumaLin(sampleSrc(uv + vec2(0.0, texel.y)));
    float l22 = lumaLin(sampleSrc(uv + vec2(texel.x, texel.y)));
    float gx = (l20 + 2.0 * l21 + l22) - (l00 + 2.0 * l01 + l02);
    float gy = (l02 + 2.0 * l12 + l22) - (l00 + 2.0 * l10 + l20);
    float g = sqrt(gx * gx + gy * gy);
    float glen = max(g, 1e-5);
    vec2 alongDir = vec2(-gy, gx) / glen;
    vec3 along = 0.5 * (sampleSrc(uv + alongDir * texel) + sampleSrc(uv - alongDir * texel));
    float edgeAmt = clamp(g * mix(3.0, 6.0, clamp(scale / 2.5, 0.0, 1.0)), 0.0, 1.0);
    return mix(soft, along, edgeAmt * 0.4);
}

vec3 upAt(ivec2 p) {
    p = clamp(p, ivec2(0), ivec2(UP_DIM - 1));
    return smUp[p.y * UP_DIM + p.x];
}

// Display-space CAS using padded shared upscaled tile (real cross-tile neighbors).
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

// Perf: sharpen with source-neighbor taps scaled toward display (approximation).
vec3 sharpenLite(vec3 c, vec2 uv, vec2 displaySize, float sharp, float scale) {
    if (sharp <= 0.001) return c;
    sharp *= mix(1.0, 0.7, clamp((scale - 1.2) / 1.3, 0.0, 1.0));
    // Neighbor offset ≈ one display pixel in UV → sample upscaled-ish via source filter.
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
        // Load source covering the WG plus UP_PAD display halo.
        fillSrcTile(groupOrig - ivec2(UP_PAD), renderSize, displaySize, tileBase);

        // Interior: always fill shared (clamp off-screen lanes so edge CAS isn't poisoned).
        {
            ivec2 dispClamped = clamp(dst, ivec2(0), ivec2(max(dw - 1, 0), max(dh - 1, 0)));
            vec2 uvI = (vec2(dispClamped) + 0.5) / displaySize;
            rgb = upsampleFromTile(uvI, renderSize, tileBase, scale);
            smUp[(lid.y + UP_PAD) * UP_DIM + (lid.x + UP_PAD)] = rgb;
        }
        barrier();

        // Halo: cooperative fill so edge CAS sees real neighbors (not clamped WG).
        int lane = int(gl_LocalInvocationIndex);
        int total = UP_DIM * UP_DIM;
        for (int i = lane; i < total; i += 256) {
            int ux = i % UP_DIM;
            int uy = i / UP_DIM;
            bool interior = ux >= UP_PAD && ux < UP_PAD + 16 && uy >= UP_PAD && uy < UP_PAD + 16;
            if (interior) continue;
            ivec2 disp = groupOrig + ivec2(ux - UP_PAD, uy - UP_PAD);
            vec2 uvH = (vec2(disp) + 0.5) / displaySize;
            // Clamp to image for out-of-bounds halo (edges of framebuffer).
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
