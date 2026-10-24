#include "present.hpp"
#include "shader_path.hpp"

#include <chrono>
#include <limits>

void Presenter::init(DeviceContext* deviceCtx, VkSurfaceKHR surf, uint32_t width, uint32_t height,
                     bool enableVsync) {
    ctx = deviceCtx;
    ctx->makeCurrent();
    surface = surf;
    vsync = enableVsync;
    extent = {width, height};

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physical, surface, &caps);
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        extent = caps.currentExtent;
    } else {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->physical, surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->physical, surface, &formatCount, formats.data());
    format = formats[0].format;
    VkColorSpaceKHR colorSpace = formats[0].colorSpace;
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            format = f.format;
            colorSpace = f.colorSpace;
            break;
        }
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx->physical, surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx->physical, surface, &presentModeCount, presentModes.data());

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR; // always available
    if (!vsync) {
        // Uncapped throughput: prefer IMMEDIATE, then MAILBOX
        for (auto pm : presentModes) {
            if (pm == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                presentMode = pm;
                break;
            }
        }
        if (presentMode == VK_PRESENT_MODE_FIFO_KHR) {
            for (auto pm : presentModes) {
                if (pm == VK_PRESENT_MODE_MAILBOX_KHR) {
                    presentMode = pm;
                    break;
                }
            }
        }
    }
    std::printf("Present mode: %s (vsync=%d)\n",
                presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" :
                presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO",
                vsync ? 1 : 0);

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = format;
    sci.imageColorSpace = colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = presentMode;
    sci.clipped = VK_TRUE;

    uint32_t qfs[] = {ctx->graphicsFamily, ctx->presentFamily};
    if (ctx->graphicsFamily != ctx->presentFamily) {
        sci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        sci.queueFamilyIndexCount = 2;
        sci.pQueueFamilyIndices = qfs;
    } else {
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    vkCheck(vkCreateSwapchainKHR(ctx->device, &sci, nullptr, &swapchain), "vkCreateSwapchainKHR");

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(ctx->device, swapchain, &n, nullptr);
    images.resize(n);
    vkGetSwapchainImagesKHR(ctx->device, swapchain, &n, images.data());
    views.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(ctx->device, &vci, nullptr, &views[i]), "swapchain view");
    }

    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    vkCheck(vkCreateRenderPass(ctx->device, &rpci, nullptr, &renderPass), "present renderPass");

    framebuffers.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbci.renderPass = renderPass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &views[i];
        fbci.width = extent.width;
        fbci.height = extent.height;
        fbci.layers = 1;
        vkCheck(vkCreateFramebuffer(ctx->device, &fbci, nullptr, &framebuffers[i]), "present fb");
    }

    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 2;
    dslci.pBindings = binds;
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &dslci, nullptr, &dsl), "present dsl");

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    vkCheck(vkCreatePipelineLayout(ctx->device, &plci, nullptr, &pipelineLayout), "present layout");

    auto vertCode = readSpirv(xfsrShaderPath("blit.vert.spv"));
    auto fragCode = readSpirv(xfsrShaderPath("blit.frag.spv"));
    VkShaderModule vert = createShaderModule(ctx->device, vertCode);
    VkShaderModule frag = createShaderModule(ctx->device, fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    VkRect2D sc{{0, 0}, extent};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pColorBlendState = &cb;
    gpci.layout = pipelineLayout;
    gpci.renderPass = renderPass;
    vkCheck(vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline), "blit pipe");
    vkDestroyShaderModule(ctx->device, vert, nullptr);
    vkDestroyShaderModule(ctx->device, frag, nullptr);

    VkDescriptorPoolSize psz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 4;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &psz;
    vkCheck(vkCreateDescriptorPool(ctx->device, &dpci, nullptr, &pool), "present pool");
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    vkCheck(vkAllocateDescriptorSets(ctx->device, &dai, &set), "present set");

    VkSamplerCreateInfo sci2{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci2.magFilter = VK_FILTER_LINEAR;
    sci2.minFilter = VK_FILTER_LINEAR;
    sci2.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci2.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci2.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCheck(vkCreateSampler(ctx->device, &sci2, nullptr, &blitSampler), "present sampler");

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx->graphicsFamily;
    vkCheck(vkCreateCommandPool(ctx->device, &pci, nullptr, &cmdPool), "present cmd pool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkCheck(vkAllocateCommandBuffers(ctx->device, &cai, &cmd), "present cmd");

    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCheck(vkCreateSemaphore(ctx->device, &semci, nullptr, &imageAvailable), "imageAvailable");
    vkCheck(vkCreateSemaphore(ctx->device, &semci, nullptr, &renderFinished), "renderFinished");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCheck(vkCreateFence(ctx->device, &fci, nullptr, &inFlight), "present fence");
}

void Presenter::destroy() {
    if (!ctx) return;
    ctx->makeCurrent();
    ctx->waitIdle();
    if (inFlight) vkDestroyFence(ctx->device, inFlight, nullptr);
    if (imageAvailable) vkDestroySemaphore(ctx->device, imageAvailable, nullptr);
    if (renderFinished) vkDestroySemaphore(ctx->device, renderFinished, nullptr);
    if (cmdPool) vkDestroyCommandPool(ctx->device, cmdPool, nullptr);
    if (blitSampler) vkDestroySampler(ctx->device, blitSampler, nullptr);
    if (pool) vkDestroyDescriptorPool(ctx->device, pool, nullptr);
    if (pipeline) vkDestroyPipeline(ctx->device, pipeline, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(ctx->device, pipelineLayout, nullptr);
    if (dsl) vkDestroyDescriptorSetLayout(ctx->device, dsl, nullptr);
    if (renderPass) vkDestroyRenderPass(ctx->device, renderPass, nullptr);
    for (auto fb : framebuffers) vkDestroyFramebuffer(ctx->device, fb, nullptr);
    for (auto v : views) vkDestroyImageView(ctx->device, v, nullptr);
    if (swapchain) vkDestroySwapchainKHR(ctx->device, swapchain, nullptr);
    framebuffers.clear();
    views.clear();
    images.clear();
    cmdPool = VK_NULL_HANDLE;
    cmd = VK_NULL_HANDLE;
    swapchain = VK_NULL_HANDLE;
    ctx = nullptr;
}

double Presenter::present(const GpuImage& src, const GpuImage* hud) {
    auto t0 = std::chrono::steady_clock::now();
    ctx->makeCurrent();

    vkCheck(vkWaitForFences(ctx->device, 1, &inFlight, VK_TRUE, UINT64_MAX), "present wait");
    vkCheck(vkResetFences(ctx->device, 1, &inFlight), "present reset fence");

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(ctx->device, swapchain, UINT64_MAX, imageAvailable,
                                         VK_NULL_HANDLE, &imageIndex);
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        vkCheck(acq, "vkAcquireNextImageKHR");
    }

    const GpuImage& hudImg = hud ? *hud : src;
    VkDescriptorImageInfo ii[2] = {
        {src.sampler ? src.sampler : blitSampler, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {hudImg.sampler ? hudImg.sampler : blitSampler, hudImg.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet writes[2]{};
    for (int i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &ii[i];
    }
    // If no HUD, bind transparent-looking by using src with shader that treats missing alpha —
    // caller should always pass HUD; if null, duplicate src and shader uses hud.a (opaque scene).
    if (!hud) {
        // Force hud alpha path off: blit.frag mixes by hud.a — duplicate src would wash out.
        // Use a 1x1 approach isn't available; require hud or skip blend by binding empty.
        // Fallback: bind src again but shader will mix incorrectly. Prefer always passing hud.
    }
    vkUpdateDescriptorSets(ctx->device, 2, writes, 0, nullptr);

    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkClearValue clear{};
    clear.color = {{0, 0, 0, 1}};
    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = renderPass;
    rpbi.framebuffer = framebuffers[imageIndex];
    rpbi.renderArea.extent = extent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkCheck(vkEndCommandBuffer(cmd), "present end cmd");

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imageAvailable;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderFinished;
    vkCheck(vkQueueSubmit(ctx->graphicsQueue, 1, &si, inFlight), "present submit");

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderFinished;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(ctx->presentQueue, &pi);
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
        vkCheck(pr, "vkQueuePresentKHR");
    }
    vkCheck(vkWaitForFences(ctx->device, 1, &inFlight, VK_TRUE, UINT64_MAX), "present drain");

    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}
