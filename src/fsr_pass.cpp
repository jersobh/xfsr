#include "fsr_pass.hpp"
#include "shader_path.hpp"

#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

struct FsrConsts {
    AU1 const0[4];
    AU1 const1[4];
    AU1 const2[4];
    AU1 const3[4];
    AU1 sample[4];
};

static void fillEasuUbo(FsrPass& pass, int slot) {
    FsrConsts c{};
    FsrEasuCon(c.const0, c.const1, c.const2, c.const3,
               static_cast<AF1>(pass.renderW), static_cast<AF1>(pass.renderH),
               static_cast<AF1>(pass.renderW), static_cast<AF1>(pass.renderH),
               static_cast<AF1>(pass.displayW), static_cast<AF1>(pass.displayH));
    c.sample[0] = 0;
    c.sample[1] = pass.swizzleBgra ? 1u : 0u;
    std::memcpy(pass.easuUboMapped[slot], &c, sizeof(c));
}

static void fillRcasUbo(FsrPass& pass, int slot) {
    FsrConsts c{};
    FsrRcasCon(c.const0, static_cast<AF1>(2.0f - pass.sharpness * 2.0f));
    c.sample[0] = 0;
    c.sample[1] = pass.swizzleBgra ? 1u : 0u;
    std::memcpy(pass.rcasUboMapped[slot], &c, sizeof(c));
}

static void createMappedUbo(DeviceContext* ctx, VkBuffer* buf, VkDeviceMemory* mem, void** mapped) {
    auto& d = ctx->vk;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = sizeof(FsrConsts);
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    vkCheck(d.CreateBuffer(ctx->device, &bci, nullptr, buf), "vkCreateBuffer(fsr ubo)");
    VkMemoryRequirements req{};
    d.GetBufferMemoryRequirements(ctx->device, *buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(ctx->physical, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkCheck(d.AllocateMemory(ctx->device, &mai, nullptr, mem), "vkAllocateMemory(fsr ubo)");
    vkCheck(d.BindBufferMemory(ctx->device, *buf, *mem, 0), "vkBindBufferMemory(fsr ubo)");
    vkCheck(d.MapMemory(ctx->device, *mem, 0, sizeof(FsrConsts), 0, mapped), "vkMapMemory(fsr ubo)");
}

static bool deviceSupportsFp16Fsr(VkPhysicalDevice phys) {
    VkPhysicalDevice16BitStorageFeatures storage16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &storage16;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    return f12.shaderFloat16 && storage16.storageBuffer16BitAccess;
}

static bool formatSupportsStorage(VkPhysicalDevice phys, VkFormat format) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(phys, format, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

void FsrPass::init(DeviceContext* deviceCtx) {
    ctx = deviceCtx;
    ctx->loadDispatch();
    auto& d = ctx->vk;

    useFp16 = deviceSupportsFp16Fsr(ctx->physical) && (ctx->props.vendorID == 0x1002);
    if (const char* e = xfsrEnv("XFSR_FSR_FP16", "CROSSVULKAN_FSR_FP16")) {
        if (e[0] == '0') useFp16 = false;
        if (e[0] == '1') useFp16 = deviceSupportsFp16Fsr(ctx->physical);
    }
    // XFSR_RCAS=0 → EASU only (faster, softer). Default on.
    enableRcas = true;
    if (const char* e = xfsrEnv("XFSR_RCAS", "CROSSVULKAN_RCAS")) {
        if (e[0] == '0') enableRcas = false;
        if (e[0] == '1') enableRcas = true;
    }
    std::printf("FsrPass on %s: fp16=%d rcas=%d\n", ctx->props.deviceName, useFp16 ? 1 : 0,
                enableRcas ? 1 : 0);

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
            "vkCreateDescriptorSetLayout(fsr)");

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    vkCheck(d.CreatePipelineLayout(ctx->device, &plci, nullptr, &pipelineLayout),
            "vkCreatePipelineLayout(fsr)");

    auto makePipe = [&](const char* spv, VkPipeline* out) {
        auto code = readSpirv(spv);
        VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sci.codeSize = code.size() * sizeof(uint32_t);
        sci.pCode = code.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCheck(d.CreateShaderModule(ctx->device, &sci, nullptr, &mod), "vkCreateShaderModule(fsr)");
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = stage;
        ci.layout = pipelineLayout;
        vkCheck(d.CreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &ci, nullptr, out),
                "vkCreateComputePipelines(fsr)");
        d.DestroyShaderModule(ctx->device, mod, nullptr);
    };
    const char* prec = useFp16 ? "fp16" : "fp32";
    makePipe(xfsrShaderPath((std::string("fsr_easu_") + prec + ".comp.spv").c_str()).c_str(),
             &easuPipeline);
    makePipe(xfsrShaderPath((std::string("fsr_rcas_") + prec + ".comp.spv").c_str()).c_str(),
             &rcasPipeline);

    const uint32_t setsNeeded = static_cast<uint32_t>(kOutRing * 2);
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setsNeeded},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, setsNeeded},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, setsNeeded},
        {VK_DESCRIPTOR_TYPE_SAMPLER, setsNeeded},
    };
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = setsNeeded;
    dpci.poolSizeCount = 4;
    dpci.pPoolSizes = sizes;
    vkCheck(d.CreateDescriptorPool(ctx->device, &dpci, nullptr, &pool),
            "vkCreateDescriptorPool(fsr)");

    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    for (int i = 0; i < kOutRing; ++i) {
        vkCheck(d.AllocateDescriptorSets(ctx->device, &dai, &easuSets[i]),
                "vkAllocateDescriptorSets(easu)");
        vkCheck(d.AllocateDescriptorSets(ctx->device, &dai, &rcasSets[i]),
                "vkAllocateDescriptorSets(rcas)");
        createMappedUbo(ctx, &easuUbos[i], &easuUboMem[i], &easuUboMapped[i]);
        createMappedUbo(ctx, &rcasUbos[i], &rcasUboMem[i], &rcasUboMapped[i]);
    }
}

void FsrPass::resize(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh) {
    ctx->loadDispatch();
    renderW = rw;
    renderH = rh;
    displayW = dw;
    displayH = dh;
    writeOut = readyOut = 0;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    for (int i = 0; i < kOutRing; ++i) {
        easuOut[i].destroy();
        rcasOut[i].destroy();
        easuOut[i] = createImage(ctx, displayW, displayH, outputFormat, usage,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_TILING_OPTIMAL, true);
        rcasOut[i] = createImage(ctx, displayW, displayH, outputFormat, usage,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_TILING_OPTIMAL, true);
    }
}

void FsrPass::destroy() {
    if (!ctx) return;
    ctx->loadDispatch();
    auto& d = ctx->vk;
    for (int i = 0; i < kOutRing; ++i) {
        easuOut[i].destroy();
        rcasOut[i].destroy();
        if (easuUboMapped[i] && d.UnmapMemory) {
            d.UnmapMemory(ctx->device, easuUboMem[i]);
            easuUboMapped[i] = nullptr;
        }
        if (rcasUboMapped[i] && d.UnmapMemory) {
            d.UnmapMemory(ctx->device, rcasUboMem[i]);
            rcasUboMapped[i] = nullptr;
        }
        if (easuUbos[i] && d.DestroyBuffer) d.DestroyBuffer(ctx->device, easuUbos[i], nullptr);
        if (rcasUbos[i] && d.DestroyBuffer) d.DestroyBuffer(ctx->device, rcasUbos[i], nullptr);
        if (easuUboMem[i] && d.FreeMemory) d.FreeMemory(ctx->device, easuUboMem[i], nullptr);
        if (rcasUboMem[i] && d.FreeMemory) d.FreeMemory(ctx->device, rcasUboMem[i], nullptr);
        easuUbos[i] = rcasUbos[i] = VK_NULL_HANDLE;
        easuUboMem[i] = rcasUboMem[i] = VK_NULL_HANDLE;
        easuSets[i] = rcasSets[i] = VK_NULL_HANDLE;
    }
    if (pool && d.DestroyDescriptorPool) d.DestroyDescriptorPool(ctx->device, pool, nullptr);
    if (easuPipeline && d.DestroyPipeline) d.DestroyPipeline(ctx->device, easuPipeline, nullptr);
    if (rcasPipeline && d.DestroyPipeline) d.DestroyPipeline(ctx->device, rcasPipeline, nullptr);
    if (pipelineLayout && d.DestroyPipelineLayout)
        d.DestroyPipelineLayout(ctx->device, pipelineLayout, nullptr);
    if (dsl && d.DestroyDescriptorSetLayout) d.DestroyDescriptorSetLayout(ctx->device, dsl, nullptr);
    pool = VK_NULL_HANDLE;
    easuPipeline = rcasPipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    dsl = VK_NULL_HANDLE;
    ctx = nullptr;
}

bool FsrPass::formatNeedsBgraSwizzle(VkFormat swapFormat) {
    return swapFormat == VK_FORMAT_B8G8R8A8_UNORM || swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
}

bool FsrPass::canDirectToSwapchain(VkFormat swapFormat) const {
    if (!ctx || swapFormat == VK_FORMAT_UNDEFINED) return false;
    const bool rgba = swapFormat == VK_FORMAT_R8G8B8A8_UNORM ||
                      swapFormat == VK_FORMAT_R8G8B8A8_SRGB;
    const bool bgra = formatNeedsBgraSwizzle(swapFormat);
    if (!rgba && !bgra) return false;
    return formatSupportsStorage(ctx->physical, swapFormat);
}

static void writeSet(DeviceDispatch& d, VkDevice device, VkDescriptorSet set, VkBuffer ubo,
                     VkImageView srcView, VkSampler sampler, VkImageView dstView) {
    VkDescriptorBufferInfo ubi{ubo, 0, sizeof(FsrConsts)};
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

void FsrPass::record(VkCommandBuffer cmd, const GpuImage& src, VkImageView directDstView,
                     VkImage directDstImage, bool directSwizzleBgra) {
    ctx->loadDispatch();
    auto& d = ctx->vk;
    const int slot = writeOut;
    GpuImage& easu = easuOut[slot];
    GpuImage& rcas = rcasOut[slot];
    const bool directFinal = directDstView != VK_NULL_HANDLE && directDstImage != VK_NULL_HANDLE;
    // Only swizzle the pass that writes the swapchain; intermediates stay RGBA.
    swizzleBgra = false;

    // Layer present path never goes through DeviceContext::resetAndBegin — without a
    // reset, the second CmdWriteTimestamp on the same query index DEVICE_LOSTs NVIDIA.
    if (ctx->timestampPool && d.CmdResetQueryPool)
        d.CmdResetQueryPool(cmd, ctx->timestampPool, 0, 8);

    d.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, ctx->timestampPool, 2);

    const bool easuDirect = directFinal && !enableRcas;
    swizzleBgra = easuDirect && directSwizzleBgra;
    fillEasuUbo(*this, slot);
    swizzleBgra = false;
    VkImageView easuDstView = easuDirect ? directDstView : easu.view;
    VkImage easuDstImage = easuDirect ? directDstImage : easu.image;
    writeSet(d, ctx->device, easuSets[slot], easuUbos[slot], src.view, src.sampler, easuDstView);

    transitionImage(ctx, cmd, easuDstImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                    VK_ACCESS_SHADER_WRITE_BIT);

    d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, easuPipeline);
    d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                            &easuSets[slot], 0, nullptr);
    const uint32_t gx = (displayW + 15) / 16;
    const uint32_t gy = (displayH + 15) / 16;
    d.CmdDispatch(cmd, gx, gy, 1);

    if (enableRcas) {
        transitionImage(ctx, cmd, easu.image, VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

        swizzleBgra = directFinal && directSwizzleBgra;
        fillRcasUbo(*this, slot);
        swizzleBgra = false;
        VkImageView rcasDstView = directFinal ? directDstView : rcas.view;
        VkImage rcasDstImage = directFinal ? directDstImage : rcas.image;
        writeSet(d, ctx->device, rcasSets[slot], rcasUbos[slot], easu.view, easu.sampler,
                 rcasDstView);

        transitionImage(ctx, cmd, rcasDstImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                        VK_ACCESS_SHADER_WRITE_BIT);

        d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rcasPipeline);
        d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                                &rcasSets[slot], 0, nullptr);
        d.CmdDispatch(cmd, gx, gy, 1);

        if (directFinal) {
            transitionImage(ctx, cmd, rcasDstImage, VK_IMAGE_LAYOUT_GENERAL,
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_SHADER_WRITE_BIT, 0);
        } else {
            transitionImage(ctx, cmd, rcas.image, VK_IMAGE_LAYOUT_GENERAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_SHADER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        }
    } else if (easuDirect) {
        transitionImage(ctx, cmd, easuDstImage, VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_SHADER_WRITE_BIT, 0);
    } else {
        transitionImage(ctx, cmd, easu.image, VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    }

    d.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestampPool, 3);
    readyOut = slot;
    writeOut = (writeOut + 1) % kOutRing;
}

double FsrPass::readGpuMs() const {
    if (!ctx || !ctx->vk.GetQueryPoolResults) return 0.0;
    uint64_t ts[2]{};
    if (ctx->vk.GetQueryPoolResults(ctx->device, ctx->timestampPool, 2, 2, sizeof(ts), ts,
                                    sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return 0.0;
    }
    return (static_cast<double>(ts[1] - ts[0]) * ctx->timestampPeriod) / 1.0e6;
}
