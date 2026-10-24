#version 450

layout(set = 0, binding = 0) uniform sampler2D srcTex;
layout(set = 0, binding = 1) uniform sampler2D hudTex;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    // Vulkan NDC Y points down, so fullscreen-triangle UVs already match
    // top-left image origins. Do not flip the HUD (CPU-rasterized top-left)
    // or glyphs appear mirrored at the bottom of the screen.
    // Scene offscreen may need a Y flip depending on viewport setup.
    vec2 sceneUV = vec2(vUV.x, 1.0 - vUV.y);
    vec4 scene = texture(srcTex, sceneUV);
    vec4 hud = texture(hudTex, vUV);
    outColor = mix(scene, hud, hud.a);
}
