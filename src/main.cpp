#include "bench.hpp"
#include "dual_devices.hpp"
#include "frame_transfer.hpp"
#include "fsr_pass.hpp"
#include "fsr3_pass.hpp"
#include "hud_overlay.hpp"
#include "present.hpp"
#include "scene_renderer.hpp"
#include "vulkan_context.hpp"

#include <chrono>
#include <iostream>
#include <sstream>
#include <vector>

namespace {

struct BenchResult {
    BenchMode mode = BenchMode::Native;
    FsrPreset preset = FsrPreset::Quality;
    int frames = 0;
    double fps = 0;
    double p50 = 0;
    double p95 = 0;
    double p99 = 0;
    double xferMs = 0;
    double memcpyMs = 0;
    double renderMs = 0;
    double dFsrMs = 0;
    double iFsrMs = 0;
};

struct App {
    VulkanContext vk;
    DualDevices devices;
    SceneRenderer scene;
    FsrPass fsrDgpu;
    FsrPass fsrIgpu;
    // FidelityFX VK backend stores a single global VkDeviceContext — only one
    // Fsr3Pass may be live. Rebind when switching dGPU ↔ iGPU.
    Fsr3Pass fsr3;
    FrameTransfer xfer;
    FrameTransfer xferMotion;
    FrameTransfer xferDepth;
    Presenter presenter;
    HudOverlay hud;
    Bench bench;

    uint32_t displayW = 1920;
    uint32_t displayH = 1080;
    uint32_t renderW = 1280;
    uint32_t renderH = 720;
    bool resourcesDirty = true;
    bool benchLocked = false;
    int complexity = 48;

    // Offload pipeline: FSR(N) overlaps next frame's dGPU+DMA. Uses integrated.fence.
    int pipeFrames = 0;

    bool wantsFsr3() const {
        return bench.mode == BenchMode::Fsr3Local || bench.mode == BenchMode::Fsr3Offload;
    }

    void bindFsr3(DeviceContext* want) {
        if (fsr3.ctx == want && fsr3.contextReady) return;
        if (fsr3.ctx) {
            fsr3.destroy();
        }
        fsr3.init(want, vk.instance);
    }

    void applyPresetSize() {
        renderSizeForPreset(bench.preset, displayW, displayH, renderW, renderH);
        if (bench.mode == BenchMode::Native) {
            renderW = displayW;
            renderH = displayH;
        }
        resourcesDirty = true;
        scene.resetHistory();
        fsr3.resetHistory();
        std::printf("\nResolution render=%ux%u display=%ux%u mode=%s preset=%s\n",
                    renderW, renderH, displayW, displayH,
                    modeName(bench.mode), presetName(bench.preset));
    }

    void ensureResources() {
        if (!resourcesDirty) return;
        devices.discrete.waitIdle();
        devices.integrated.waitIdle();

        scene.resize(renderW, renderH);
        fsrDgpu.resize(renderW, renderH, displayW, displayH);
        fsrIgpu.resize(renderW, renderH, displayW, displayH);

        if (bench.mode == BenchMode::Fsr3Local) {
            bindFsr3(&devices.discrete);
            fsr3.resize(renderW, renderH, displayW, displayH);
        } else if (bench.mode == BenchMode::Fsr3Offload) {
            bindFsr3(&devices.integrated);
            fsr3.resize(renderW, renderH, displayW, displayH);
        }

        if (bench.mode == BenchMode::Fsr3Offload) {
            xfer.resize(renderW, renderH, VK_FORMAT_R8G8B8A8_UNORM, 4);
            xferMotion.resize(renderW, renderH, VK_FORMAT_R16G16_SFLOAT, 4);
            xferDepth.resize(renderW, renderH, VK_FORMAT_R32_SFLOAT, 4);
        } else if (bench.mode == BenchMode::FsrOffload) {
            xfer.resize(renderW, renderH, VK_FORMAT_R8G8B8A8_UNORM, 4);
        } else {
            xfer.resize(displayW, displayH, VK_FORMAT_R8G8B8A8_UNORM, 4);
        }

        resourcesDirty = false;
        bench.resetStats();
        pipeFrames = 0;
    }

    bool validateVegaFsr() {
        std::printf("Phase 2: validating FSR on integrated GPU...\n");
        xfer.resize(renderW, renderH, VK_FORMAT_R8G8B8A8_UNORM, 4);
        auto& dstOpt = xfer.dstOptimal();

        // Paint a gradient into host-visible staging (shared host ptr or linear image).
        if (xfer.useSharedHost && xfer.hostPtr()) {
            auto* dst = static_cast<uint8_t*>(xfer.hostPtr());
            for (uint32_t y = 0; y < renderH; ++y) {
                for (uint32_t x = 0; x < renderW; ++x) {
                    uint8_t* p = dst + (static_cast<size_t>(y) * renderW + x) * 4;
                    p[0] = static_cast<uint8_t>(x * 255 / renderW);
                    p[1] = static_cast<uint8_t>(y * 255 / renderH);
                    p[2] = 128;
                    p[3] = 255;
                }
            }
        } else {
            auto& dstLin = xfer.dstLinear();
            auto* dst = static_cast<uint8_t*>(dstLin.mapped) + dstLin.offset;
            for (uint32_t y = 0; y < renderH; ++y) {
                for (uint32_t x = 0; x < renderW; ++x) {
                    uint8_t* p = dst + y * dstLin.rowPitch + x * 4;
                    p[0] = static_cast<uint8_t>(x * 255 / renderW);
                    p[1] = static_cast<uint8_t>(y * 255 / renderH);
                    p[2] = 128;
                    p[3] = 255;
                }
            }
            dstLin.flushMapped();
        }

        devices.integrated.resetAndBegin();
        transitionImage(devices.integrated.cmd, dstOpt.image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        if (xfer.useSharedHost) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {renderW, renderH, 1};
            vkCmdCopyBufferToImage(devices.integrated.cmd, xfer.slots[0].dstBuf, dstOpt.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        } else {
            auto& dstLin = xfer.dstLinear();
            transitionImage(devices.integrated.cmd, dstLin.image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_HOST_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT);
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {renderW, renderH, 1};
            vkCmdCopyImage(devices.integrated.cmd, dstLin.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dstOpt.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        transitionImage(devices.integrated.cmd, dstOpt.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT);
        fsrIgpu.record(devices.integrated.cmd, dstOpt);
        devices.integrated.endSubmitWait();
        double ms = fsrIgpu.readGpuMs();
        std::printf("Phase 2 OK: Vega FSR GPU time ≈ %.3f ms (device-local clock)\n", ms);
        return true;
    }

    bool validateTransfer() {
        std::printf("Phase 3: validating cross-GPU transfer...\n");
        scene.time = 0.0f;
        scene.complexity = 8;
        devices.discrete.resetAndBegin();
        scene.record(devices.discrete.cmd);
        devices.discrete.endSubmitWait();

        xfer.resize(renderW, renderH, VK_FORMAT_R8G8B8A8_UNORM, 4);
        double memcpyMs = 0, totalMs = 0;
        const GpuImage& dst = xfer.transferSync(scene.color, memcpyMs, totalMs);

        bool ok = false;
        if (xfer.useSharedHost && xfer.hostPtr()) {
            // Same host allocation on both devices — sample a few non-zero pixels.
            const auto* p = static_cast<const uint8_t*>(xfer.hostPtr());
            uint64_t sum = 0;
            const size_t n = std::min(xfer.hostBytes(), size_t{4096});
            for (size_t i = 0; i < n; ++i) sum += p[i];
            ok = sum != 0;
            std::printf("Phase 3: shared_host ok=%d  bytes=%zu sampleSum=%llu  "
                        "cpu_memcpy=%.2fms total=%.2fms  dst=%ux%u\n",
                        ok ? 1 : 0, xfer.hostBytes(),
                        static_cast<unsigned long long>(sum), memcpyMs, totalMs, dst.width,
                        dst.height);
        } else {
            uint64_t crcSrc = 0, crcDst = 0;
            const auto& srcLin = xfer.srcLinear();
            const auto& dstLin = xfer.dstLinear();
            const auto* s = static_cast<const uint8_t*>(srcLin.mapped) + srcLin.offset;
            const auto* d = static_cast<const uint8_t*>(dstLin.mapped) + dstLin.offset;
            for (uint32_t y = 0; y < renderH; ++y) {
                for (uint32_t x = 0; x < renderW * 4; ++x) {
                    crcSrc = crcSrc * 1315423911u + s[y * srcLin.rowPitch + x];
                    crcDst = crcDst * 1315423911u + d[y * dstLin.rowPitch + x];
                }
            }
            ok = (crcSrc == crcDst) && (crcSrc != 0);
            std::printf("Phase 3: memcpy CRC src=%llu dst=%llu match=%d  memcpy=%.2fms "
                        "total=%.2fms  dst=%ux%u\n",
                        static_cast<unsigned long long>(crcSrc),
                        static_cast<unsigned long long>(crcDst), ok ? 1 : 0, memcpyMs, totalMs,
                        dst.width, dst.height);
        }
        scene.complexity = complexity;
        return ok;
    }

    void frame() {
        ensureResources();
        auto t0 = std::chrono::steady_clock::now();

        BenchSample sample{};
        const GpuImage* presentSrc = nullptr;

        scene.time = static_cast<float>(bench.frameIndex) * 0.016f;
        scene.complexity = complexity;
        scene.enableJitter = wantsFsr3();
        if (scene.enableJitter) {
            fsr3.getJitter(scene.jitterX, scene.jitterY);
            fsr3.cameraNear = scene.cameraNear;
            fsr3.cameraFar = scene.cameraFar;
            fsr3.cameraFovY = scene.cameraFovY;
            fsr3.lastFrameMs =
                bench.last.cpuFrameMs > 0.0 ? static_cast<float>(bench.last.cpuFrameMs) : 16.0f;
        } else {
            scene.jitterX = scene.jitterY = 0.0f;
        }

        devices.discrete.resetAndBegin();
        scene.record(devices.discrete.cmd);

        if (bench.mode == BenchMode::FsrLocal) {
            fsrDgpu.record(devices.discrete.cmd, scene.color);
        } else if (bench.mode == BenchMode::Fsr3Local) {
            fsr3.record(devices.discrete.cmd, scene.color, scene.depth, scene.motion);
        }
        devices.discrete.endSubmitWait();
        sample.dgpuRenderMs = scene.readGpuMs();
        if (bench.mode == BenchMode::FsrLocal) {
            sample.dgpuFsrMs = fsrDgpu.readGpuMs();
        }

        const GpuImage* toTransfer = &scene.color;
        if (bench.mode == BenchMode::FsrLocal) {
            toTransfer = &fsrDgpu.output();
        } else if (bench.mode == BenchMode::Fsr3Local) {
            toTransfer = &fsr3.output();
        }

        if (bench.mode == BenchMode::FsrOffload) {
            // Pipeline: transferSync(N) overlaps in-flight FSR(N-1) on the iGPU.
            // Then wait FSR(N-1) → present it → kick FSR(N) without waiting.
            // IMPORTANT: submit must signal devices.integrated.fence (same fence resetAndBegin waits on).
            const GpuImage& onIgpu = xfer.transferSync(*toTransfer, sample.transferMemcpyMs,
                                                      sample.transferTotalMs);

            devices.integrated.waitFence(devices.integrated.fence);
            if (pipeFrames >= 1) {
                sample.igpuFsrMs = fsrIgpu.readGpuMs();
                presentSrc = &fsrIgpu.output();
            }

            devices.integrated.resetAndBegin(); // waits (already signaled) + resets fence
            fsrIgpu.record(devices.integrated.cmd, onIgpu);
            devices.integrated.endSubmit(devices.integrated.fence); // async

            if (pipeFrames == 0) {
                devices.integrated.waitFence(devices.integrated.fence);
                sample.igpuFsrMs = fsrIgpu.readGpuMs();
                presentSrc = &fsrIgpu.output();
            }
            ++pipeFrames;
        } else if (bench.mode == BenchMode::Fsr3Offload) {
            double mc = 0, tot = 0;
            const GpuImage& colorI = xfer.transferSync(scene.color, mc, tot);
            sample.transferMemcpyMs += mc;
            sample.transferTotalMs += tot;
            const GpuImage& depthI = xferDepth.transferSync(scene.depthExport, mc, tot);
            sample.transferMemcpyMs += mc;
            sample.transferTotalMs += tot;
            const GpuImage& motionI = xferMotion.transferSync(scene.motion, mc, tot);
            sample.transferMemcpyMs += mc;
            sample.transferTotalMs += tot;

            devices.integrated.resetAndBegin();
            fsr3.record(devices.integrated.cmd, colorI, depthI, motionI);
            devices.integrated.endSubmitWait();
            presentSrc = &fsr3.output();
        } else {
            const GpuImage& onIgpu = xfer.transferSync(*toTransfer, sample.transferMemcpyMs,
                                                      sample.transferTotalMs);
            presentSrc = &onIgpu;
        }

        hud.setText([&]() {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "FPS %.1f  (building frame...)\n"
                          "mode %s  preset %s  vsync %d\n"
                          "render %.2f  xfer %.2f  (memcpy %.2f)\n"
                          "dFSR %.2f  iFSR %.2f\n"
                          "p50 %.1f  p95 %.1f  p99 %.1f ms\n"
                          "3D grid %dx%d  shade %d  keys 0-4 Q/W/E",
                          bench.fps(), modeName(bench.mode), presetName(bench.preset),
                          bench.vsync ? 1 : 0,
                          sample.dgpuRenderMs, sample.transferTotalMs, sample.transferMemcpyMs,
                          sample.dgpuFsrMs, sample.igpuFsrMs,
                          bench.percentileMs(0.50), bench.percentileMs(0.95), bench.percentileMs(0.99),
                          scene.gridSize, scene.gridSize, complexity);
            return std::string(buf);
        }());
        hud.upload();
        sample.presentMs = presenter.present(*presentSrc, &hud.image);

        auto t1 = std::chrono::steady_clock::now();
        sample.cpuFrameMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        bench.push(sample);
        glfwSetWindowTitle(vk.window, bench.formatTitle().c_str());
        bench.printHud();
    }
};

App* gApp = nullptr;

void onKey(GLFWwindow* window, int key, int, int action, int) {
    if (action != GLFW_PRESS || !gApp) return;
    App& a = *gApp;
    if (key == GLFW_KEY_ESCAPE) {
        glfwSetWindowShouldClose(window, 1);
        return;
    }
    if (a.benchLocked) return;
    switch (key) {
    case GLFW_KEY_0:
        a.bench.mode = BenchMode::Native;
        a.applyPresetSize();
        break;
    case GLFW_KEY_1:
        a.bench.mode = BenchMode::FsrLocal;
        a.applyPresetSize();
        break;
    case GLFW_KEY_2:
        a.bench.mode = BenchMode::FsrOffload;
        a.applyPresetSize();
        break;
    case GLFW_KEY_3:
        a.bench.mode = BenchMode::Fsr3Local;
        a.applyPresetSize();
        break;
    case GLFW_KEY_4:
        a.bench.mode = BenchMode::Fsr3Offload;
        a.applyPresetSize();
        break;
    case GLFW_KEY_Q:
        a.bench.preset = FsrPreset::Quality;
        a.applyPresetSize();
        break;
    case GLFW_KEY_W:
        a.bench.preset = FsrPreset::Balanced;
        a.applyPresetSize();
        break;
    case GLFW_KEY_E:
        a.bench.preset = FsrPreset::Performance;
        a.applyPresetSize();
        break;
    case GLFW_KEY_SPACE:
        a.bench.dumpCsvLine();
        break;
    case GLFW_KEY_EQUAL:
    case GLFW_KEY_KP_ADD:
        a.complexity = std::min(128, a.complexity + 8);
        std::printf("\ncomplexity=%d\n", a.complexity);
        break;
    case GLFW_KEY_MINUS:
    case GLFW_KEY_KP_SUBTRACT:
        a.complexity = std::max(8, a.complexity - 8);
        std::printf("\ncomplexity=%d\n", a.complexity);
        break;
    case GLFW_KEY_LEFT_BRACKET:
        a.scene.gridSize = std::max(1, a.scene.gridSize - 2);
        std::printf("\ngridSize=%d (%d cubes)\n", a.scene.gridSize,
                    a.scene.gridSize * a.scene.gridSize);
        break;
    case GLFW_KEY_RIGHT_BRACKET:
        a.scene.gridSize = std::min(11, a.scene.gridSize + 2);
        std::printf("\ngridSize=%d (%d cubes)\n", a.scene.gridSize,
                    a.scene.gridSize * a.scene.gridSize);
        break;
    case GLFW_KEY_R:
        a.bench.resetStats();
        std::printf("\nStats reset\n");
        break;
    default:
        break;
    }
}

struct SweepConfig {
    std::vector<BenchMode> modes;
    std::vector<FsrPreset> presets;
    bool all = false;
};

SweepConfig parseBenchSpec(const std::string& spec) {
    SweepConfig cfg;
    if (spec.empty() || spec == "all") {
        cfg.all = true;
        cfg.modes = {BenchMode::Native, BenchMode::FsrLocal, BenchMode::FsrOffload,
                     BenchMode::Fsr3Local, BenchMode::Fsr3Offload};
        cfg.presets = {FsrPreset::Quality, FsrPreset::Balanced, FsrPreset::Performance};
        return cfg;
    }

    // "1,3,4" or "1,3:quality,balanced" — modes[, presets via :]
    std::string modePart = spec;
    std::string presetPart;
    auto colon = spec.find(':');
    if (colon != std::string::npos) {
        modePart = spec.substr(0, colon);
        presetPart = spec.substr(colon + 1);
    }

    std::stringstream ss(modePart);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        int m = std::atoi(tok.c_str());
        if (m < 0 || m > 4) throw std::runtime_error("Invalid --bench mode: " + tok);
        cfg.modes.push_back(static_cast<BenchMode>(m));
    }
    if (cfg.modes.empty()) {
        cfg.modes = {BenchMode::FsrLocal, BenchMode::Fsr3Local, BenchMode::Fsr3Offload};
    }

    if (presetPart.empty()) {
        cfg.presets = {FsrPreset::Quality}; // single preset unless specified
    } else {
        std::stringstream ps(presetPart);
        while (std::getline(ps, tok, ',')) {
            if (tok == "quality" || tok == "q") cfg.presets.push_back(FsrPreset::Quality);
            else if (tok == "balanced" || tok == "w") cfg.presets.push_back(FsrPreset::Balanced);
            else if (tok == "performance" || tok == "e") cfg.presets.push_back(FsrPreset::Performance);
            else throw std::runtime_error("Invalid preset in --bench: " + tok);
        }
    }
    if (cfg.presets.empty()) cfg.presets = {FsrPreset::Quality};
    return cfg;
}

void printResultsTable(const std::vector<BenchResult>& results) {
    if (results.empty()) {
        std::printf("\nNo bench results collected.\n");
        return;
    }

    std::printf("\n"
                "╔══════════════════════════════════════════════════════════════════════════════════════╗\n"
                "║                         xfsr — bench results                                  ║\n"
                "╠════════════════╦═════════════╦═══════╦════════╦════════╦════════╦════════╦══════════╣\n"
                "║ mode           ║ preset      ║  FPS  ║  p50   ║  p95   ║  p99   ║  xfer  ║ memcpy   ║\n"
                "╠════════════════╬═════════════╬═══════╬════════╬════════╬════════╬════════╬══════════╣\n");

    const BenchResult* best = nullptr;
    for (const auto& r : results) {
        std::printf("║ %-14s ║ %-11s ║ %5.1f ║ %6.1f ║ %6.1f ║ %6.1f ║ %6.1f ║ %8.1f ║\n",
                    modeName(r.mode), presetName(r.preset), r.fps, r.p50, r.p95, r.p99,
                    r.xferMs, r.memcpyMs);
        if (r.frames > 0 && (!best || r.fps > best->fps)) best = &r;
    }

    std::printf("╚════════════════╩═════════════╩═══════╩════════╩════════╩════════╩════════╩══════════╝\n");

    if (best) {
        std::printf("Best FPS: %s / %s  →  %.1f FPS  (p50=%.1fms xfer=%.1fms)\n",
                    modeName(best->mode), presetName(best->preset), best->fps, best->p50,
                    best->xferMs);
    }

    // Native is always full-res (one row); compare every FSR config against it.
    const BenchResult* native = nullptr;
    for (const auto& r : results) {
        if (r.mode == BenchMode::Native && r.fps > 0) {
            native = &r;
            break;
        }
    }
    if (native) {
        std::printf("\nRelative to native (%.1f FPS full-res):\n", native->fps);
        for (const auto& r : results) {
            if (r.mode == BenchMode::Native) continue;
            double pct = (r.fps / native->fps - 1.0) * 100.0;
            std::printf("  %s/%s: %.1f FPS  (%+.1f%%)\n",
                        modeName(r.mode), presetName(r.preset), r.fps, pct);
        }
    }
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    bool runValidate = false;
    bool vsync = false;
    int maxFrames = -1;
    bool runBench = false;
    std::string benchSpec = "all";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--validate") runValidate = true;
        if (a == "--vsync") vsync = true;
        if (a == "--frames" && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
        if (a == "--bench") {
            runBench = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                benchSpec = argv[++i];
            }
        }
    }
    if (runBench && maxFrames < 0) maxFrames = 300;

    try {
        setvbuf(stdout, nullptr, _IONBF, 0);
        App app;
        gApp = &app;
        app.bench.vsync = vsync;
        app.vk.init(app.displayW, app.displayH, "xfsr — dual-GPU FSR demo");
        glfwSetKeyCallback(app.vk.window, onKey);

        app.devices.init(app.vk.instance, app.vk.surface);
        if (!app.devices.integrated.canPresent) {
            throw std::runtime_error("Integrated GPU cannot present — fair benchmark path unavailable");
        }
        app.devices.presentDevice = &app.devices.integrated;

        app.scene.init(&app.devices.discrete, app.renderW, app.renderH);
        app.fsrDgpu.init(&app.devices.discrete);
        app.fsrIgpu.init(&app.devices.integrated);
        // FSR3 bound lazily in ensureResources (FFX global device limit)
        app.xfer.init(&app.devices.discrete, &app.devices.integrated);
        app.xferMotion.init(&app.devices.discrete, &app.devices.integrated);
        app.xferDepth.init(&app.devices.discrete, &app.devices.integrated);
        app.presenter.init(app.devices.presentDevice, app.vk.surface, app.displayW, app.displayH, vsync);
        app.hud.init(app.devices.presentDevice, app.displayW, app.displayH);

        app.bench.mode = modeFromEnv();
        app.bench.preset = presetFromEnv();
        std::printf("Env defaults: XFSR_FSR → %s  XFSR_PRESET → %s"
                    "  (XFSR_OFFLOAD default=1)\n",
                    modeName(app.bench.mode), presetName(app.bench.preset));
        app.applyPresetSize();
        app.ensureResources();

        if (runValidate) {
            if (!app.validateVegaFsr()) return 2;
            if (!app.validateTransfer()) return 3;
            std::printf("Validation passed. Starting interactive demo.\n");
            app.resourcesDirty = true;
            app.ensureResources();
        }

        std::printf(
            "\nControls:\n"
            "  0 native | 1 FSR1 local | 2 FSR1 offload | 3 FSR3.1 local | 4 FSR3.1 offload (Vega)\n"
            "  Q quality 1.5x | W balanced 1.7x | E performance 2.0x\n"
            "  +/- shading cost | [/] cube grid size | R reset | Space CSV | Esc quit\n"
            "  --bench all|--bench 1,3,4[:quality,balanced] --frames N\n"
            "Env: XFSR_FSR=1|3  XFSR_PRESET=quality|balanced|performance\n"
            "     XFSR_OFFLOAD=0|1  (Steam: XFSR_FSR=1 XFSR_PRESET=performance …)\n"
            "Present GPU: %s (same for all modes)\n\n",
            app.devices.presentDevice->props.deviceName);

        auto wantStop = [&]() {
            if (glfwGetKey(app.vk.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) return true;
            if (!app.benchLocked && app.vk.shouldClose()) return true;
            if (app.benchLocked && app.vk.shouldClose()) {
                glfwSetWindowShouldClose(app.vk.window, 0);
            }
            return false;
        };

        auto runOne = [&](int framesPerMode) -> BenchResult {
            BenchResult r;
            r.mode = app.bench.mode;
            r.preset = app.bench.preset;
            for (int i = 0; i < 60 && !wantStop(); ++i) {
                app.vk.poll();
                app.frame();
            }
            if (wantStop()) return r;
            app.bench.resetStats();
            std::printf("\nWarmup done — measuring %s/%s (%d frames)...\n",
                        modeName(app.bench.mode), presetName(app.bench.preset), framesPerMode);
            int measured = 0;
            while (!wantStop()) {
                app.vk.poll();
                app.frame();
                ++measured;
                if (framesPerMode > 0 && measured >= framesPerMode) break;
            }
            r.frames = measured;
            if (measured > 0) {
                app.bench.dumpCsvLine();
                r.fps = app.bench.fps();
                r.p50 = app.bench.percentileMs(0.50);
                r.p95 = app.bench.percentileMs(0.95);
                r.p99 = app.bench.percentileMs(0.99);
                r.xferMs = app.bench.last.transferTotalMs;
                r.memcpyMs = app.bench.last.transferMemcpyMs;
                r.renderMs = app.bench.last.dgpuRenderMs;
                r.dFsrMs = app.bench.last.dgpuFsrMs;
                r.iFsrMs = app.bench.last.igpuFsrMs;
                std::printf(
                    "Done %s/%s: %d frames, FPS=%.1f p50=%.2f p95=%.2f p99=%.2f "
                    "xfer=%.2f (memcpy %.2f)\n",
                    modeName(r.mode), presetName(r.preset), measured, r.fps, r.p50, r.p95, r.p99,
                    r.xferMs, r.memcpyMs);
            }
            return r;
        };

        std::vector<BenchResult> results;

        if (runBench) {
            app.benchLocked = true;
            SweepConfig sweep = parseBenchSpec(benchSpec);
            const int total = static_cast<int>(sweep.modes.size() * sweep.presets.size());
            std::printf("Bench sweep: %d configs (%zu modes × %zu presets), %d frames each\n",
                        total, sweep.modes.size(), sweep.presets.size(), maxFrames);
            int idx = 0;
            for (BenchMode m : sweep.modes) {
                for (FsrPreset p : sweep.presets) {
                    if (wantStop()) break;
                    ++idx;
                    // Native only needs one preset slot (full-res); still run each for fair CSV
                    // but skip duplicate native×balanced/performance (same res).
                    if (m == BenchMode::Native && p != FsrPreset::Quality) {
                        std::printf("\n[%d/%d] skip native/%s (same as quality full-res)\n",
                                    idx, total, presetName(p));
                        continue;
                    }
                    std::printf("\n======== [%d/%d] %s / %s ========\n",
                                idx, total, modeName(m), presetName(p));
                    app.bench.mode = m;
                    app.bench.preset = p;
                    app.applyPresetSize();
                    app.ensureResources();
                    BenchResult r = runOne(maxFrames);
                    if (r.frames > 0) results.push_back(r);
                }
                if (wantStop()) break;
            }
            app.benchLocked = false;
            printResultsTable(results);
        } else {
            runOne(maxFrames);
        }

        app.hud.destroy();
        app.presenter.destroy();
        app.fsr3.destroy();
        app.xferDepth.destroy();
        app.xferMotion.destroy();
        app.xfer.destroy();
        app.fsrIgpu.destroy();
        app.fsrDgpu.destroy();
        app.scene.destroy();
        app.devices.destroy();
        app.vk.destroy();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
