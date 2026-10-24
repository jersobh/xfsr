#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 viewProj;
    mat4 prevViewProj;
    vec4 camPos;
    vec4 lightDir; // xyz + time
    vec4 params;   // x = complexity
} ubo;

layout(std430, set = 0, binding = 1) readonly buffer PrevModels {
    mat4 prevModel[];
} prevBuf;

layout(push_constant) uniform Push {
    mat4 model;
    float tint;
    uint instanceId;
    float pad1;
    float pad2;
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vColor;
layout(location = 3) out vec4 vCurrClip;
layout(location = 4) out vec4 vPrevClip;

void main() {
    vec4 world = pc.model * vec4(inPos, 1.0);
    mat4 prevM = prevBuf.prevModel[pc.instanceId];
    vec4 prevWorld = prevM * vec4(inPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(pc.model) * inNormal;
    float s = pc.tint;
    vColor = 0.45 + 0.55 * fract(vec3(s * 0.17, s * 0.31, s * 0.53));
    vCurrClip = ubo.viewProj * world;
    vPrevClip = ubo.prevViewProj * prevWorld;
    gl_Position = vCurrClip;
}
