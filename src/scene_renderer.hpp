#pragma once

#include "dual_devices.hpp"
#include "gpu_image.hpp"
#include "math.hpp"

struct SceneRenderer {
    DeviceContext* ctx = nullptr;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    GpuImage color;
    GpuImage motion;      // RG16F UV-space motion (prevUv - currUv)
    GpuImage depthExport;  // R32F color-export of gl_FragCoord.z (host-transferable)
    GpuImage depth;       // D32 for Z-test; used by FSR3 local
    VkFramebuffer framebuffer = VK_NULL_HANDLE;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    VkBuffer ubo = VK_NULL_HANDLE;
    VkDeviceMemory uboMemory = VK_NULL_HANDLE;
    void* uboMapped = nullptr;
    VkBuffer prevModelSsbo = VK_NULL_HANDLE;
    VkDeviceMemory prevModelMemory = VK_NULL_HANDLE;
    void* prevModelMapped = nullptr;
    uint32_t indexCount = 0;

    uint32_t width = 0;
    uint32_t height = 0;
    float time = 0.0f;
    int complexity = 48;
    int gridSize = 5; // NxN rotating cubes + ground plane

    // Temporal / FSR3 inputs
    bool enableJitter = false;
    float jitterX = 0.0f; // pixel offsets (FSR GetJitterOffset space)
    float jitterY = 0.0f;
    bool hasPrev = false;
    Mat4 prevViewProj = Mat4::identity();
    // Previous per-instance models for the current grid layout (recomputed each frame).
    // Indexed as cubes then ground.
    static constexpr int kMaxInstances = 11 * 11 + 1;
    Mat4 prevModels[kMaxInstances]{};
    Mat4 currModels[kMaxInstances]{};
    int instanceCount = 0;

    float cameraNear = 0.1f;
    float cameraFar = 100.0f;
    float cameraFovY = 60.0f * 3.14159265f / 180.0f;

    void init(DeviceContext* deviceCtx, uint32_t w, uint32_t h);
    void resize(uint32_t w, uint32_t h);
    void destroy();
    void resetHistory();
    void record(VkCommandBuffer cmd);
    double readGpuMs() const;
};
