#include "edge_pass.hpp"
#include "shader_path.hpp"

#include <cstdio>
#include <cstring>

namespace {

struct EdgeConsts {
    float sizes[4]; // renderW, renderH, displayW, displayH
    float opts[4];  // sharpness, swizzleBgra, mode(0=quality 1=perf), scale
};

float parseFloat01(const char* e) {
    float v = 0.f, frac = 0.f, place = 0.1f;
    const char* p = e;
    while (*p == ' ' || *p == '\t') ++p;
    while (*p >= '0' && *p <= '9') {
        v = v * 10.f + static_cast<float>(*p - '0');
        ++p;
    }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            frac += static_cast<float>(*p - '0') * place;
            place *= 0.1f;
            ++p;
        }
    }
    return v + frac;
}

bool parsePerfModeEnv(const char* e) {
    if (!e || !e[0]) return true;
    if (e[0] == '1') return true;
    if (std::strcmp(e, "perf") == 0 || std::strcmp(e, "lite") == 0 ||
        std::strcmp(e, "edge-lite") == 0 || std::strcmp(e, "edge_lite") == 0)
        return true;
    if (e[0] == '0' || std::strcmp(e, "quality") == 0 || std::strcmp(e, "hq") == 0)
        return false;
    return true;
}

void createMappedUbo(DeviceContext* ctx, VkBuffer* buf, VkDeviceMemory* mem, void** mapped) {
    auto& d = ctx->vk;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = sizeof(EdgeConsts);
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    vkCheck(d.CreateBuffer(ctx->device, &bci, nullptr, buf), "vkCreateBuffer(edge ubo)");
    VkMemoryRequirements req{};
    d.GetBufferMemoryRequirements(ctx->device, *buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(ctx->physical, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkCheck(d.AllocateMemory(ctx->device, &mai, nullptr, mem), "vkAllocateMemory(edge ubo)");
    vkCheck(d.BindBufferMemory(ctx->device, *buf, *mem, 0), "vkBindBufferMemory(edge ubo)");
    vkCheck(d.MapMemory(ctx->device, *mem, 0, sizeof(EdgeConsts), 0, mapped),
            "vkMapMemory(edge ubo)");
}

bool formatSupportsStorage(VkPhysicalDevice phys, VkFormat format) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(phys, format, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

void writeSet(DeviceDispatch& d, VkDevice device, VkDescriptorSet set, VkBuffer ubo,
              VkImageView srcView, VkSampler sampler, VkImageView dstView) {
    VkDescriptorBufferInfo ubi{ubo, 0, sizeof(EdgeConsts)};
    VkDescriptorImageInfo srcInfo{sampler, srcView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, dstView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo sampInfo{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};

    VkWriteDescriptorSet writes[4]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &ubi;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[1].pImageInfo = &srcInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &dstInfo;

    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[3].pImageInfo = &sampInfo;

    d.UpdateDescriptorSets(device, 4, writes, 0, nullptr);
}

void fillUbo(EdgePass& pass, int slot, bool swizzle) {
    EdgeConsts c{};
    c.sizes[0] = static_cast<float>(pass.renderW);
    c.sizes[1] = static_cast<float>(pass.renderH);
    c.sizes[2] = static_cast<float>(pass.displayW);
    c.sizes[3] = static_cast<float>(pass.displayH);
    c.opts[0] = pass.sharpness;
    c.opts[1] = swizzle ? 1.f : 0.f;
    c.opts[2] = pass.perfMode ? 1.f : 0.f;
    const float sx = pass.renderW > 0 ? static_cast<float>(pass.displayW) /
                                            static_cast<float>(pass.renderW)
                                      : 1.f;
    const float sy = pass.renderH > 0 ? static_cast<float>(pass.displayH) /
                                            static_cast<float>(pass.renderH)
                                      : 1.f;
    c.opts[3] = sx > sy ? sx : sy;
    std::memcpy(pass.uboMapped[slot], &c, sizeof(c));
}

} // namespace

void EdgePass::init(DeviceContext* deviceCtx) {
    ctx = deviceCtx;
    ctx->loadDispatch();
    auto& d = ctx->vk;

    if (const char* e = xfsrEnv("XFSR_EDGE_SHARPNESS", "CROSSVULKAN_EDGE_SHARPNESS")) {
        float v = parseFloat01(e);
        if (v >= 0.f && v <= 1.f) sharpness = v;
    }
    if (const char* e = xfsrEnv("XFSR_EDGE_MODE", "CROSSVULKAN_EDGE_MODE")) {
        perfMode = parsePerfModeEnv(e);
    }
    std::printf("EdgePass on %s: sharpness=%.2f mode=%s\n", ctx->props.deviceName, sharpness,
                perfMode ? "perf" : "quality");

    VkDescriptorSetLayoutBinding binds[4]{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    binds[2].binding = 2;
    binds[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binds[2].descriptorCount = 1;
    binds[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    binds[3].binding = 3;
    binds[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    binds[3].descriptorCount = 1;
    binds[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 4;
    dslci.pBindings = binds;
    vkCheck(d.CreateDescriptorSetLayout(ctx->device, &dslci, nullptr, &dsl),
            "vkCreateDescriptorSetLayout(edge)");

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    vkCheck(d.CreatePipelineLayout(ctx->device, &plci, nullptr, &pipelineLayout),
            "vkCreatePipelineLayout(edge)");

    auto code = readSpirv(xfsrShaderPath("edge_pass.comp.spv").c_str());
    VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sci.codeSize = code.size() * sizeof(uint32_t);
    sci.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCheck(d.CreateShaderModule(ctx->device, &sci, nullptr, &mod), "vkCreateShaderModule(edge)");
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stage;
    ci.layout = pipelineLayout;
    vkCheck(d.CreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline),
            "vkCreateComputePipelines(edge)");
    d.DestroyShaderModule(ctx->device, mod, nullptr);

    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kOutRing},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kOutRing},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kOutRing},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kOutRing},
    };
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = kOutRing;
    dpci.poolSizeCount = 4;
    dpci.pPoolSizes = sizes;
    vkCheck(d.CreateDescriptorPool(ctx->device, &dpci, nullptr, &pool),
            "vkCreateDescriptorPool(edge)");

    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    for (int i = 0; i < kOutRing; ++i) {
        vkCheck(d.AllocateDescriptorSets(ctx->device, &dai, &sets[i]),
                "vkAllocateDescriptorSets(edge)");
        createMappedUbo(ctx, &ubos[i], &uboMem[i], &uboMapped[i]);
    }
}

void EdgePass::resize(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh) {
    ctx->loadDispatch();
    renderW = rw;
    renderH = rh;
    displayW = dw;
    displayH = dh;
    writeOut = readyOut = 0;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    for (int i = 0; i < kOutRing; ++i) {
        out[i].destroy();
        out[i] = createImage(ctx, displayW, displayH, outputFormat, usage,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_TILING_OPTIMAL, true);
    }
}

void EdgePass::destroy() {
    if (!ctx) return;
    ctx->loadDispatch();
    auto& d = ctx->vk;
    for (int i = 0; i < kOutRing; ++i) {
        out[i].destroy();
        if (uboMapped[i] && d.UnmapMemory) {
            d.UnmapMemory(ctx->device, uboMem[i]);
            uboMapped[i] = nullptr;
        }
        if (ubos[i] && d.DestroyBuffer) d.DestroyBuffer(ctx->device, ubos[i], nullptr);
        if (uboMem[i] && d.FreeMemory) d.FreeMemory(ctx->device, uboMem[i], nullptr);
        ubos[i] = VK_NULL_HANDLE;
        uboMem[i] = VK_NULL_HANDLE;
        sets[i] = VK_NULL_HANDLE;
    }
    if (pool && d.DestroyDescriptorPool) d.DestroyDescriptorPool(ctx->device, pool, nullptr);
    if (pipeline && d.DestroyPipeline) d.DestroyPipeline(ctx->device, pipeline, nullptr);
    if (pipelineLayout && d.DestroyPipelineLayout)
        d.DestroyPipelineLayout(ctx->device, pipelineLayout, nullptr);
    if (dsl && d.DestroyDescriptorSetLayout) d.DestroyDescriptorSetLayout(ctx->device, dsl, nullptr);
    pool = VK_NULL_HANDLE;
    pipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    dsl = VK_NULL_HANDLE;
    ctx = nullptr;
}

void EdgePass::setSharpness(float s) {
    if (s < 0.f) s = 0.f;
    if (s > 1.f) s = 1.f;
    sharpness = s;
}

void EdgePass::setPerfMode(bool perf) { perfMode = perf; }

bool EdgePass::formatNeedsBgraSwizzle(VkFormat swapFormat) {
    return swapFormat == VK_FORMAT_B8G8R8A8_UNORM || swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
}

bool EdgePass::canDirectToSwapchain(VkFormat swapFormat) const {
    if (!ctx || swapFormat == VK_FORMAT_UNDEFINED) return false;
    const bool rgba =
        swapFormat == VK_FORMAT_R8G8B8A8_UNORM || swapFormat == VK_FORMAT_R8G8B8A8_SRGB;
    const bool bgra = formatNeedsBgraSwizzle(swapFormat);
    if (!rgba && !bgra) return false;
    return formatSupportsStorage(ctx->physical, swapFormat);
}

void EdgePass::record(VkCommandBuffer cmd, const GpuImage& src, VkImageView directDstView,
                      VkImage directDstImage, bool directSwizzleBgra) {
    ctx->loadDispatch();
    auto& d = ctx->vk;
    const int slot = writeOut;
    GpuImage& dst = out[slot];
    const bool directFinal = directDstView != VK_NULL_HANDLE && directDstImage != VK_NULL_HANDLE;
    const bool swizzle = directFinal && directSwizzleBgra;

    fillUbo(*this, slot, swizzle);

    VkImageView dstView = directFinal ? directDstView : dst.view;
    VkImage dstImage = directFinal ? directDstImage : dst.image;
    writeSet(d, ctx->device, sets[slot], ubos[slot], src.view, src.sampler, dstView);

    transitionImage(ctx, cmd, dstImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                    VK_ACCESS_SHADER_WRITE_BIT);

    d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &sets[slot],
                            0, nullptr);
    const uint32_t gx = (displayW + 15) / 16;
    const uint32_t gy = (displayH + 15) / 16;
    d.CmdDispatch(cmd, gx, gy, 1);

    if (directFinal) {
        transitionImage(ctx, cmd, dstImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT, 0);
    } else {
        transitionImage(ctx, cmd, dst.image, VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    }

    readyOut = slot;
    writeOut = (writeOut + 1) % kOutRing;
}
