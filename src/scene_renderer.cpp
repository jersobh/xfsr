#include "scene_renderer.hpp"
#include "shader_path.hpp"

#include <cmath>
#include <cstring>
#include <vector>

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
};

struct SceneUBO {
    Mat4 viewProj;
    Mat4 prevViewProj;
    float camPos[4];
    float lightDir[4]; // xyz + time in w
    float params[4];   // x = complexity, yz = jitter pixels
};

struct ScenePush {
    Mat4 model;
    float tint;
    uint32_t instanceId;
    float pad[2];
};

static void createHostBuffer(DeviceContext* ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                             VkBuffer* buf, VkDeviceMemory* mem, void** mapped) {
    ctx->makeCurrent();
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    vkCheck(vkCreateBuffer(ctx->device, &bci, nullptr, buf), "vkCreateBuffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(ctx->device, *buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(ctx->physical, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkCheck(vkAllocateMemory(ctx->device, &mai, nullptr, mem), "vkAllocateMemory(buf)");
    vkCheck(vkBindBufferMemory(ctx->device, *buf, *mem, 0), "vkBindBufferMemory");
    if (mapped) {
        vkCheck(vkMapMemory(ctx->device, *mem, 0, size, 0, mapped), "vkMapMemory(buf)");
    }
}

static void buildCubeMesh(std::vector<Vertex>& verts, std::vector<uint16_t>& indices) {
    struct Face { Vec3 n; Vec3 a, b, c, d; };
    const Face faces[] = {
        {{0, 0, 1},  {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}},
        {{0, 0,-1},  { 1,-1,-1},{-1,-1,-1},{-1, 1,-1},{ 1, 1,-1}},
        {{0, 1, 0},  {-1, 1, 1},{ 1, 1, 1},{ 1, 1,-1},{-1, 1,-1}},
        {{0,-1, 0},  {-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1},{-1,-1, 1}},
        {{1, 0, 0},  { 1,-1, 1},{ 1,-1,-1},{ 1, 1,-1},{ 1, 1, 1}},
        {{-1,0, 0},  {-1,-1,-1},{-1,-1, 1},{-1, 1, 1},{-1, 1,-1}},
    };
    for (const auto& f : faces) {
        uint16_t base = static_cast<uint16_t>(verts.size());
        auto add = [&](Vec3 p) { verts.push_back({p.x, p.y, p.z, f.n.x, f.n.y, f.n.z}); };
        add(f.a); add(f.b); add(f.c); add(f.d);
        indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
        indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 3);
    }
}

void SceneRenderer::resetHistory() {
    hasPrev = false;
    prevViewProj = Mat4::identity();
    instanceCount = 0;
}

void SceneRenderer::init(DeviceContext* deviceCtx, uint32_t w, uint32_t h) {
    ctx = deviceCtx;
    ctx->makeCurrent();
    width = w;
    height = h;

    std::vector<Vertex> verts;
    std::vector<uint16_t> indices;
    buildCubeMesh(verts, indices);
    indexCount = static_cast<uint32_t>(indices.size());

    void* vmap = nullptr;
    void* imap = nullptr;
    createHostBuffer(ctx, verts.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     &vertexBuffer, &vertexMemory, &vmap);
    std::memcpy(vmap, verts.data(), verts.size() * sizeof(Vertex));
    createHostBuffer(ctx, indices.size() * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     &indexBuffer, &indexMemory, &imap);
    std::memcpy(imap, indices.data(), indices.size() * sizeof(uint16_t));

    createHostBuffer(ctx, sizeof(SceneUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     &ubo, &uboMemory, &uboMapped);
    createHostBuffer(ctx, sizeof(Mat4) * kMaxInstances,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     &prevModelSsbo, &prevModelMemory, &prevModelMapped);

    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 2;
    dslci.pBindings = binds;
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &dslci, nullptr, &dsl), "scene dsl");

    VkDescriptorPoolSize psz[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
    };
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 2;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = psz;
    vkCheck(vkCreateDescriptorPool(ctx->device, &dpci, nullptr, &descPool), "scene pool");
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    vkCheck(vkAllocateDescriptorSets(ctx->device, &dai, &descSet), "scene set");
    VkDescriptorBufferInfo ubi{ubo, 0, sizeof(SceneUBO)};
    VkDescriptorBufferInfo sbi{prevModelSsbo, 0, sizeof(Mat4) * kMaxInstances};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &ubi;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &sbi;
    vkUpdateDescriptorSets(ctx->device, 2, writes, 0, nullptr);

    // color + motion + R32F depth export + D32 depth
    VkAttachmentDescription atts[4]{};
    atts[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    atts[1].format = VK_FORMAT_R16G16_SFLOAT;
    atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    atts[2].format = VK_FORMAT_R32_SFLOAT;
    atts[2].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    atts[3].format = VK_FORMAT_D32_SFLOAT;
    atts[3].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[3].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[3].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRefs[3] = {
        {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    };
    VkAttachmentReference depthRef{3, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 3;
    sub.pColorAttachments = colorRefs;
    sub.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkSubpassDependency depOut{};
    depOut.srcSubpass = 0;
    depOut.dstSubpass = VK_SUBPASS_EXTERNAL;
    depOut.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    depOut.dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    depOut.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depOut.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkSubpassDependency deps[] = {dep, depOut};
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 4;
    rpci.pAttachments = atts;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
    vkCheck(vkCreateRenderPass(ctx->device, &rpci, nullptr, &renderPass), "vkCreateRenderPass(scene)");

    auto vertCode = readSpirv(xfsrShaderPath("scene.vert.spv"));
    auto fragCode = readSpirv(xfsrShaderPath("scene.frag.spv"));
    VkShaderModule vert = createShaderModule(ctx->device, vertCode);
    VkShaderModule frag = createShaderModule(ctx->device, fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};

    VkVertexInputBindingDescription bind{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, px)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blendAtt[3]{};
    blendAtt[0].colorWriteMask = 0xF;
    blendAtt[1].colorWriteMask = 0xF;
    blendAtt[2].colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 3;
    cb.pAttachments = blendAtt;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(ScenePush);

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
    vkCheck(vkCreatePipelineLayout(ctx->device, &plci, nullptr, &pipelineLayout), "scene layout");

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = pipelineLayout;
    gpci.renderPass = renderPass;
    vkCheck(vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline),
            "scene pipeline");

    vkDestroyShaderModule(ctx->device, vert, nullptr);
    vkDestroyShaderModule(ctx->device, frag, nullptr);
    resize(w, h);
}

void SceneRenderer::resize(uint32_t w, uint32_t h) {
    ctx->makeCurrent();
    width = w;
    height = h;
    if (framebuffer) {
        vkDestroyFramebuffer(ctx->device, framebuffer, nullptr);
        framebuffer = VK_NULL_HANDLE;
    }
    color.destroy();
    motion.destroy();
    depthExport.destroy();
    depth.destroy();

    color = createImage(ctx->physical, ctx->device, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_TILING_OPTIMAL, true);

    motion = createImage(ctx->physical, ctx->device, width, height, VK_FORMAT_R16G16_SFLOAT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_TILING_OPTIMAL, true);

    depthExport = createImage(ctx->physical, ctx->device, width, height, VK_FORMAT_R32_SFLOAT,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_TILING_OPTIMAL, true);

    depth.device = ctx->device;
    depth.phys = ctx->physical;
    depth.width = width;
    depth.height = height;
    depth.format = VK_FORMAT_D32_SFLOAT;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_D32_SFLOAT;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCheck(vkCreateImage(ctx->device, &ici, nullptr, &depth.image), "depth image");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx->device, depth.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(ctx->physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkCheck(vkAllocateMemory(ctx->device, &mai, nullptr, &depth.memory), "depth mem");
    vkCheck(vkBindImageMemory(ctx->device, depth.image, depth.memory, 0), "depth bind");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = depth.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_D32_SFLOAT;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    vkCheck(vkCreateImageView(ctx->device, &vci, nullptr, &depth.view), "depth view");

    VkImageView attachments[] = {color.view, motion.view, depthExport.view, depth.view};
    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbci.renderPass = renderPass;
    fbci.attachmentCount = 4;
    fbci.pAttachments = attachments;
    fbci.width = width;
    fbci.height = height;
    fbci.layers = 1;
    vkCheck(vkCreateFramebuffer(ctx->device, &fbci, nullptr, &framebuffer), "scene fb");

    resetHistory();
}

void SceneRenderer::destroy() {
    if (!ctx) return;
    ctx->makeCurrent();
    ctx->waitIdle();
    if (framebuffer) vkDestroyFramebuffer(ctx->device, framebuffer, nullptr);
    color.destroy();
    motion.destroy();
    depthExport.destroy();
    depth.destroy();
    if (pipeline) vkDestroyPipeline(ctx->device, pipeline, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(ctx->device, pipelineLayout, nullptr);
    if (dsl) vkDestroyDescriptorSetLayout(ctx->device, dsl, nullptr);
    if (descPool) vkDestroyDescriptorPool(ctx->device, descPool, nullptr);
    if (renderPass) vkDestroyRenderPass(ctx->device, renderPass, nullptr);
    if (uboMapped) { vkUnmapMemory(ctx->device, uboMemory); uboMapped = nullptr; }
    if (prevModelMapped) { vkUnmapMemory(ctx->device, prevModelMemory); prevModelMapped = nullptr; }
    if (ubo) vkDestroyBuffer(ctx->device, ubo, nullptr);
    if (uboMemory) vkFreeMemory(ctx->device, uboMemory, nullptr);
    if (prevModelSsbo) vkDestroyBuffer(ctx->device, prevModelSsbo, nullptr);
    if (prevModelMemory) vkFreeMemory(ctx->device, prevModelMemory, nullptr);
    if (vertexBuffer) vkDestroyBuffer(ctx->device, vertexBuffer, nullptr);
    if (indexBuffer) vkDestroyBuffer(ctx->device, indexBuffer, nullptr);
    if (vertexMemory) vkFreeMemory(ctx->device, vertexMemory, nullptr);
    if (indexMemory) vkFreeMemory(ctx->device, indexMemory, nullptr);
    framebuffer = VK_NULL_HANDLE;
    pipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    renderPass = VK_NULL_HANDLE;
    dsl = VK_NULL_HANDLE;
    descPool = VK_NULL_HANDLE;
    descSet = VK_NULL_HANDLE;
    ubo = prevModelSsbo = vertexBuffer = indexBuffer = VK_NULL_HANDLE;
    uboMemory = prevModelMemory = vertexMemory = indexMemory = VK_NULL_HANDLE;
    ctx = nullptr;
}

void SceneRenderer::record(VkCommandBuffer cmd) {
    // Present/hud leave global volk on the iGPU; must rebind before dGPU volk cmds.
    ctx->makeCurrent();
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ctx->timestampPool, 0);

    float aspect = static_cast<float>(width) / static_cast<float>(std::max(1u, height));
    Mat4 proj = Mat4::perspective(cameraFovY, aspect, cameraNear, cameraFar);
    if (enableJitter) {
        // FSR jitter is in pixels; convert to clip-space NDC offsets.
        proj.m[8] += 2.0f * jitterX / static_cast<float>(std::max(1u, width));
        proj.m[9] += -2.0f * jitterY / static_cast<float>(std::max(1u, height));
    }

    float camR = 9.0f;
    Vec3 eye{std::cos(time * 0.35f) * camR, 4.5f + std::sin(time * 0.2f) * 1.2f,
             std::sin(time * 0.35f) * camR};
    Mat4 view = Mat4::lookAt(eye, {0, 0.5f, 0}, {0, 1, 0});
    Mat4 viewProj = proj * view;

    // Build this frame's instance models
    instanceCount = 0;
    const int n = gridSize;
    const int half = n / 2;
    for (int z = -half; z <= half; ++z) {
        for (int x = -half; x <= half; ++x) {
            if (instanceCount >= kMaxInstances - 1) break;
            float fx = static_cast<float>(x);
            float fz = static_cast<float>(z);
            float phase = fx * 0.7f + fz * 1.1f;
            float bob = std::sin(time * 1.5f + phase) * 0.35f;
            float spin = time * (1.0f + 0.15f * std::abs(fx + fz));
            currModels[instanceCount++] =
                Mat4::translate({fx * 2.2f, bob, fz * 2.2f}) *
                Mat4::rotateY(spin) * Mat4::rotateX(spin * 0.6f) *
                Mat4::scale({0.7f, 0.7f, 0.7f});
        }
    }
    currModels[instanceCount++] = Mat4::translate({0, -1.4f, 0}) * Mat4::scale({12.0f, 0.08f, 12.0f});

    // Upload previous (or current if first frame) models for MV generation.
    Mat4* prevGpu = static_cast<Mat4*>(prevModelMapped);
    for (int i = 0; i < instanceCount; ++i) {
        prevGpu[i] = hasPrev ? prevModels[i] : currModels[i];
    }

    SceneUBO u{};
    u.viewProj = viewProj;
    u.prevViewProj = hasPrev ? prevViewProj : viewProj;
    u.camPos[0] = eye.x; u.camPos[1] = eye.y; u.camPos[2] = eye.z;
    u.lightDir[0] = 0.45f; u.lightDir[1] = -1.0f; u.lightDir[2] = 0.35f; u.lightDir[3] = time;
    u.params[0] = static_cast<float>(complexity);
    u.params[1] = jitterX;
    u.params[2] = jitterY;
    std::memcpy(uboMapped, &u, sizeof(u));

    VkClearValue clears[4]{};
    clears[0].color = {{0.04f, 0.05f, 0.08f, 1.0f}};
    clears[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clears[2].color = {{1.0f, 0.0f, 0.0f, 0.0f}}; // far depth
    clears[3].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = renderPass;
    rpbi.framebuffer = framebuffer;
    rpbi.renderArea.extent = {width, height};
    rpbi.clearValueCount = 4;
    rpbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descSet, 0, nullptr);
    VkViewport vp{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &off);
    vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT16);

    int idx = 0;
    for (int z = -half; z <= half; ++z) {
        for (int x = -half; x <= half; ++x) {
            if (idx >= instanceCount - 1) break;
            ScenePush push{};
            push.model = currModels[idx];
            push.tint = static_cast<float>((x + 10) * 17 + (z + 10) * 31);
            push.instanceId = static_cast<uint32_t>(idx);
            vkCmdPushConstants(cmd, pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
            ++idx;
        }
    }

    {
        ScenePush push{};
        push.model = currModels[idx];
        push.tint = 99.0f;
        push.instanceId = static_cast<uint32_t>(idx);
        vkCmdPushConstants(cmd, pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestampPool, 1);

    prevViewProj = viewProj;
    for (int i = 0; i < instanceCount; ++i) prevModels[i] = currModels[i];
    hasPrev = true;
}

double SceneRenderer::readGpuMs() const {
    uint64_t ts[2]{};
    if (vkGetQueryPoolResults(ctx->device, ctx->timestampPool, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return 0.0;
    }
    return (static_cast<double>(ts[1] - ts[0]) * ctx->timestampPeriod) / 1.0e6;
}
