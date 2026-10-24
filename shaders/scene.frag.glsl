#version 450

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vColor;
layout(location = 3) in vec4 vCurrClip;
layout(location = 4) in vec4 vPrevClip;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 viewProj;
    mat4 prevViewProj;
    vec4 camPos;
    vec4 lightDir;
    vec4 params;
} ubo;

layout(push_constant) uniform Push {
    mat4 model;
    float tint;
    uint instanceId;
    float pad1;
    float pad2;
} pc;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;
layout(location = 2) out float outDepth; // R32F export for cross-GPU FSR3 (host-transferable)

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(-ubo.lightDir.xyz);
    vec3 V = normalize(ubo.camPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);

    float ndotl = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 64.0);

    float t = ubo.lightDir.w;
    vec2 uv = vWorldPos.xz * 2.0;
    float acc = 0.0;
    int iters = int(ubo.params.x);
    if (pc.tint > 90.0) iters = max(iters / 2, 8);

    for (int i = 0; i < 128; ++i) {
        if (i >= iters) break;
        float fi = float(i);
        vec2 q = uv * (1.0 + 0.12 * fi) + vec2(sin(t + fi), cos(t * 0.7 + fi));
        acc += noise(q) * (1.0 / (1.0 + fi * 0.1));
    }
    acc /= max(float(iters) * 0.15, 1.0);

    vec3 albedo = vColor * (0.65 + 0.35 * acc);
    if (pc.tint > 90.0) albedo = vec3(0.18, 0.2, 0.24) * (0.8 + 0.2 * acc);

    vec3 ambient = 0.12 * albedo;
    vec3 diffuse = albedo * ndotl;
    vec3 specular = vec3(0.9, 0.95, 1.0) * spec * 0.45;
    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0);
    vec3 color = ambient + diffuse + specular + rim * vec3(0.15, 0.25, 0.4);
    outColor = vec4(pow(color, vec3(0.92)), 1.0);

    // UV-space motion (prevUv - currUv); FSR motionVectorScale = render size.
    vec2 currUv = (vCurrClip.xy / max(vCurrClip.w, 1e-6)) * 0.5 + 0.5;
    vec2 prevUv = (vPrevClip.xy / max(vPrevClip.w, 1e-6)) * 0.5 + 0.5;
    outMotion = prevUv - currUv;
    outDepth = gl_FragCoord.z;
}
