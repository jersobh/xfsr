// Uncapped Vulkan load for xfsr-run. Renders a heavy fullscreen pass at the
// swapchain extent (so caps-shrink / FSR proxy actually reduces work).
// IMMEDIATE present — not vsync-capped like vkcube.
//
// Usage:
//   ./build/xfsr_load_vk [--seconds N] [--complexity N] [--width W] [--height H]
//   xfsr-run --no-zink --preset performance ./build/xfsr_load_vk --seconds 8

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void die(const char* msg) {
    std::fprintf(stderr, "xfsr_load_vk: %s\n", msg);
    std::exit(1);
}
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "xfsr_load_vk: %s failed (%d)\n", what, static_cast<int>(r));
        std::exit(1);
    }
}

uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    die("no memory type");
    return 0;
}

const char* kVert = R"(#version 450
layout(location=0) out vec2 uv;
void main() {
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Raymarched rotating cube + optional expensive shading (complexity).
// Fullscreen pass so xfsr caps-shrink still reduces real GPU work.
const char* kFrag = R"(#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform PC { float time; int complexity; float aspect; int _pad; } pc;

mat3 rotY(float a) {
  float c = cos(a), s = sin(a);
  return mat3(c,0,s, 0,1,0, -s,0,c);
}
mat3 rotX(float a) {
  float c = cos(a), s = sin(a);
  return mat3(1,0,0, 0,c,-s, 0,s,c);
}
float sdBox(vec3 p, vec3 b) {
  vec3 q = abs(p) - b;
  return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}
vec3 faceColor(vec3 n) {
  vec3 an = abs(n);
  if (an.x > an.y && an.x > an.z) return n.x > 0.0 ? vec3(0.95,0.25,0.20) : vec3(0.20,0.75,0.35);
  if (an.y > an.z) return n.y > 0.0 ? vec3(0.95,0.85,0.20) : vec3(0.55,0.35,0.95);
  return n.z > 0.0 ? vec3(0.25,0.55,0.95) : vec3(0.95,0.45,0.15);
}
void main() {
  vec2 p = uv * 2.0 - 1.0;
  p.x *= max(pc.aspect, 0.1);
  vec3 ro = vec3(0.0, 0.35, 3.2);
  vec3 rd = normalize(vec3(p, -1.6));
  mat3 R = rotY(pc.time * 0.9) * rotX(pc.time * 0.45);
  float t = 0.0;
  float hit = -1.0;
  vec3 pos;
  for (int i = 0; i < 64; ++i) {
    pos = ro + rd * t;
    float d = sdBox(R * pos, vec3(0.85));
    if (d < 0.001) { hit = t; break; }
    t += d;
    if (t > 20.0) break;
  }
  vec3 col = vec3(0.12, 0.13, 0.16);
  float ft = (-1.2 - ro.y) / rd.y;
  if (ft > 0.0 && (hit < 0.0 || ft < hit)) {
    vec3 fp = ro + rd * ft;
    float chk = mod(floor(fp.x * 1.5) + floor(fp.z * 1.5), 2.0);
    col = mix(vec3(0.18), vec3(0.28), chk) * exp(-0.08 * ft);
  }
  if (hit > 0.0) {
    pos = ro + rd * hit;
    vec3 q = R * pos;
    vec3 sn = normalize(sign(q) * step(vec3(0.84), abs(q)));
    vec3 n = normalize(R * sn);
    vec3 albedo = faceColor(sn);
    vec3 light = normalize(vec3(0.6, 0.9, 0.4));
    float diff = clamp(dot(n, light), 0.0, 1.0);
    col = albedo * (0.28 + 0.85 * diff);
    int nC = max(pc.complexity, 1);
    float v = 0.0;
    vec2 s = q.xy;
    for (int i = 0; i < nC; ++i) {
      float fi = float(i);
      s = abs(s) / (dot(s, s) + 0.001) - vec2(0.7 + 0.01 * sin(pc.time + fi));
      v += exp(-dot(s, s));
    }
    col += 0.04 * vec3(v / float(nC));
  }
  outColor = vec4(col, 1.0);
}
)";

VkShaderModule makeShader(VkDevice device, const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

// Compile GLSL via glslang/glslc at runtime is heavy — embed prebuilt? Prefer
// shipping SPIR-V as string from a tiny offline path. For simplicity we use
// glslc if available at build time via CMake embedding; here we load from env
// or compile with system glslc once.
std::vector<uint32_t> compileGlsl(const char* stage, const char* src) {
    char inPath[] = "/tmp/xfsr_load_XXXXXX";
    int fd = mkstemp(inPath);
    if (fd < 0) die("mkstemp");
    FILE* f = fdopen(fd, "w");
    if (!f) die("fdopen");
    std::fputs(src, f);
    std::fclose(f);
    std::string outPath = std::string(inPath) + ".spv";
    std::string cmd = std::string("glslc -O -fshader-stage=") + stage + " -o " + outPath + " " + inPath +
                      " 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) {
        std::remove(inPath);
        die("glslc failed (install shaderc/glslc)");
    }
    FILE* spv = std::fopen(outPath.c_str(), "rb");
    if (!spv) die("open spirv");
    std::fseek(spv, 0, SEEK_END);
    long sz = std::ftell(spv);
    std::fseek(spv, 0, SEEK_SET);
    if (sz <= 0 || (sz % 4) != 0) die("bad spirv size");
    std::vector<uint32_t> code(static_cast<size_t>(sz) / 4);
    if (std::fread(code.data(), 1, static_cast<size_t>(sz), spv) != static_cast<size_t>(sz))
        die("read spirv");
    std::fclose(spv);
    std::remove(inPath);
    std::remove(outPath.c_str());
    return code;
}

} // namespace

int main(int argc, char** argv) {
    int seconds = 8;
    int complexity = 64;
    int winW = 1920, winH = 1080;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (a == "--complexity" && i + 1 < argc) complexity = std::atoi(argv[++i]);
        else if (a == "--width" && i + 1 < argc) winW = std::atoi(argv[++i]);
        else if (a == "--height" && i + 1 < argc) winH = std::atoi(argv[++i]);
        else if (a == "-h" || a == "--help") {
            std::printf("Usage: xfsr_load_vk [--seconds N] [--complexity N] [--width W] [--height H]\n");
            return 0;
        }
    }
    complexity = std::clamp(complexity, 1, 512);

    if (!glfwInit()) die("glfwInit");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    GLFWwindow* window =
        glfwCreateWindow(winW, winH, "xfsr_load_vk (uncapped)", nullptr, nullptr);
    if (!window) die("glfwCreateWindow");

    check(volkInitialize(), "volkInitialize");

    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xfsr_load_vk";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = glfwExtCount;
    ici.ppEnabledExtensionNames = glfwExts;
    VkInstance instance = VK_NULL_HANDLE;
    check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
    volkLoadInstance(instance);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    check(glfwCreateWindowSurface(instance, window, nullptr, &surface), "surface");

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(instance, &pdCount, nullptr);
    if (!pdCount) die("no GPU");
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(instance, &pdCount, pds.data());
    VkPhysicalDevice phys = pds[0];
    for (auto p : pds) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            phys = p;
            break;
        }
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    std::printf("GPU: %s\n", props.deviceName);

    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qCount, qprops.data());
    uint32_t graphicsFamily = UINT32_MAX;
    for (uint32_t i = 0; i < qCount; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &present);
        if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            graphicsFamily = i;
            break;
        }
    }
    if (graphicsFamily == UINT32_MAX) die("no graphics+present queue");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = graphicsFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dex[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dex;
    VkDevice device = VK_NULL_HANDLE;
    check(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice");
    volkLoadDevice(device);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, graphicsFamily, 0, &queue);

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmtCount, formats.data());
    VkSurfaceFormatKHR surfFmt = formats[0];
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            surfFmt = f;
            break;
        }
    }
    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> pmodes(pmCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pmCount, pmodes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto pm : pmodes) {
        if (pm == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            presentMode = pm;
            break;
        }
    }
    if (presentMode == VK_PRESENT_MODE_FIFO_KHR) {
        for (auto pm : pmodes)
            if (pm == VK_PRESENT_MODE_MAILBOX_KHR) presentMode = pm;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(static_cast<uint32_t>(winW), caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(winH), caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }
    std::printf("swapchain %ux%u present=%s complexity=%d\n", extent.width, extent.height,
                presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR
                    ? "IMMEDIATE"
                    : (presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO"),
                complexity);

    uint32_t imageCount = std::max(2u, caps.minImageCount);
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount + 1, caps.maxImageCount);

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = surfFmt.format;
    sci.imageColorSpace = surfFmt.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = presentMode;
    sci.clipped = VK_TRUE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    check(vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain), "vkCreateSwapchainKHR");

    uint32_t nImg = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &nImg, nullptr);
    std::vector<VkImage> images(nImg);
    vkGetSwapchainImagesKHR(device, swapchain, &nImg, images.data());
    std::vector<VkImageView> views(nImg);
    for (uint32_t i = 0; i < nImg; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = surfFmt.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &vci, nullptr, &views[i]), "vkCreateImageView");
    }

    VkAttachmentDescription att{};
    att.format = surfFmt.format;
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
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    check(vkCreateRenderPass(device, &rpci, nullptr, &renderPass), "vkCreateRenderPass");

    std::vector<VkFramebuffer> fbs(nImg);
    for (uint32_t i = 0; i < nImg; ++i) {
        VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbci.renderPass = renderPass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &views[i];
        fbci.width = extent.width;
        fbci.height = extent.height;
        fbci.layers = 1;
        check(vkCreateFramebuffer(device, &fbci, nullptr, &fbs[i]), "vkCreateFramebuffer");
    }

    auto vertSpv = compileGlsl("vertex", kVert);
    auto fragSpv = compileGlsl("fragment", kFrag);
    VkShaderModule vertMod = makeShader(device, vertSpv);
    VkShaderModule fragMod = makeShader(device, fragSpv);

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = 16; // time, complexity, aspect, pad

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout), "pipelineLayout");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

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
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blendAtt;

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
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline),
          "vkCreateGraphicsPipelines");
    vkDestroyShaderModule(device, vertMod, nullptr);
    vkDestroyShaderModule(device, fragMod, nullptr);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = graphicsFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(device, &pci, nullptr, &pool), "cmdPool");

    constexpr uint32_t kFrames = 2;
    VkCommandBuffer cmds[kFrames]{};
    VkSemaphore imageAvail[kFrames]{}, renderDone[kFrames]{};
    VkFence fences[kFrames]{};
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kFrames;
    check(vkAllocateCommandBuffers(device, &cai, cmds), "alloc cmds");
    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kFrames; ++i) {
        check(vkCreateSemaphore(device, &semci, nullptr, &imageAvail[i]), "sem");
        check(vkCreateSemaphore(device, &semci, nullptr, &renderDone[i]), "sem2");
        check(vkCreateFence(device, &fci, nullptr, &fences[i]), "fence");
    }

    auto t0 = std::chrono::steady_clock::now();
    auto tReport = t0;
    uint64_t frames = 0;
    uint64_t framesWindow = 0;
    double bestFps = 0, lastFps = 0;
    uint32_t frameSlot = 0;
    float time = 0.0f;

    std::printf("XFSR_BENCH_BEGIN api=vulkan seconds=%d complexity=%d extent=%ux%u\n", seconds,
                complexity, extent.width, extent.height);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - t0).count() >= seconds) break;

        uint32_t slot = frameSlot;
        check(vkWaitForFences(device, 1, &fences[slot], VK_TRUE, UINT64_MAX), "wait");
        check(vkResetFences(device, 1, &fences[slot]), "reset fence");

        uint32_t imageIndex = 0;
        VkResult acq = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvail[slot],
                                             VK_NULL_HANDLE, &imageIndex);
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) break;
        if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) check(acq, "acquire");

        check(vkResetCommandBuffer(cmds[slot], 0), "reset cmd");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmds[slot], &bi), "begin");

        VkClearValue clear{};
        clear.color = {{0.12f, 0.13f, 0.16f, 1.0f}};
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = renderPass;
        rpbi.framebuffer = fbs[imageIndex];
        rpbi.renderArea.extent = extent;
        rpbi.clearValueCount = 1;
        rpbi.pClearValues = &clear;
        vkCmdBeginRenderPass(cmds[slot], &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmds[slot], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        struct {
            float time;
            int complexity;
            float aspect;
            int pad;
        } pc{time, complexity,
             extent.height ? static_cast<float>(extent.width) / static_cast<float>(extent.height)
                           : 1.0f,
             0};
        vkCmdPushConstants(cmds[slot], pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                           &pc);
        vkCmdDraw(cmds[slot], 3, 1, 0, 0);
        vkCmdEndRenderPass(cmds[slot]);
        check(vkEndCommandBuffer(cmds[slot]), "end");

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &imageAvail[slot];
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmds[slot];
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderDone[slot];
        check(vkQueueSubmit(queue, 1, &si, fences[slot]), "submit");

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderDone[slot];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &imageIndex;
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR) break;

        frameSlot = (slot + 1) % kFrames;
        ++frames;
        ++framesWindow;
        time += 0.016f;

        double dt = std::chrono::duration<double>(now - tReport).count();
        if (dt >= 0.5) {
            lastFps = framesWindow / dt;
            bestFps = std::max(bestFps, lastFps);
            std::printf("FPS %.1f  (frames=%llu extent=%ux%u)\n", lastFps,
                        static_cast<unsigned long long>(frames), extent.width, extent.height);
            framesWindow = 0;
            tReport = now;
        }
    }

    vkDeviceWaitIdle(device);
    double totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double avgFps = totalSec > 0 ? frames / totalSec : 0;
    std::printf("XFSR_BENCH_END api=vulkan frames=%llu seconds=%.3f avg_fps=%.2f best_fps=%.2f "
                "extent=%ux%u complexity=%d\n",
                static_cast<unsigned long long>(frames), totalSec, avgFps, bestFps, extent.width,
                extent.height, complexity);

    // Minimal teardown — process exit is fine for a bench tool.
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
