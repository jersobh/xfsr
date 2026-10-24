#include "bench.hpp"

#include <numeric>

void renderSizeForPreset(FsrPreset preset, uint32_t displayW, uint32_t displayH,
                         uint32_t& outW, uint32_t& outH) {
    // Per-dimension scale factors from AMD FSR 1.0 docs
    float scale = presetScale(preset);
    outW = static_cast<uint32_t>(std::round(static_cast<float>(displayW) / scale));
    outH = static_cast<uint32_t>(std::round(static_cast<float>(displayH) / scale));
    // Known exact targets for 1920x1080
    if (displayW == 1920 && displayH == 1080) {
        switch (preset) {
        case FsrPreset::Quality: outW = 1280; outH = 720; break;
        case FsrPreset::Balanced: outW = 1129; outH = 635; break;
        case FsrPreset::Performance: outW = 960; outH = 540; break;
        }
    }
    outW = std::max(64u, outW);
    outH = std::max(64u, outH);
}

void Bench::push(const BenchSample& s) {
    last = s;
    frameMs.push_back(s.cpuFrameMs);
    while (frameMs.size() > kWindow) frameMs.pop_front();
    ++frameIndex;
}

double Bench::meanMs() const {
    if (frameMs.empty()) return 0;
    double sum = std::accumulate(frameMs.begin(), frameMs.end(), 0.0);
    return sum / static_cast<double>(frameMs.size());
}

double Bench::percentileMs(double p) const {
    if (frameMs.empty()) return 0;
    std::vector<double> sorted(frameMs.begin(), frameMs.end());
    std::sort(sorted.begin(), sorted.end());
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

double Bench::fps() const {
    double m = meanMs();
    return m > 0.0 ? 1000.0 / m : 0.0;
}

std::string Bench::formatOverlay() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "FPS %.1f  (cpu %.1f ms)\n"
                  "mode %s  preset %s  vsync %d\n"
                  "render %.2f  xfer %.2f  (memcpy %.2f)\n"
                  "dFSR %.2f  iFSR %.2f  present %.2f\n"
                  "p50 %.1f  p95 %.1f  p99 %.1f ms\n"
                  "keys 0/1/2  Q/W/E  +/-  Space=CSV",
                  fps(), meanMs(),
                  modeName(mode), presetName(preset), vsync ? 1 : 0,
                  last.dgpuRenderMs, last.transferTotalMs, last.transferMemcpyMs,
                  last.dgpuFsrMs, last.igpuFsrMs, last.presentMs,
                  percentileMs(0.50), percentileMs(0.95), percentileMs(0.99));
    return buf;
}

std::string Bench::formatTitle() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "xfsr | %.1f FPS | %s/%s | render %.1fms xfer %.1fms",
                  fps(), modeName(mode), presetName(preset),
                  last.dgpuRenderMs, last.transferTotalMs);
    return buf;
}

void Bench::printHud(bool forceNewline) const {
    if (forceNewline || (frameIndex % 30) == 0) {
        std::printf(
            "\n--- frame %llu ---\n"
            "  FPS=%.1f  cpu=%.2fms  p50=%.2f  p95=%.2f  p99=%.2f\n"
            "  mode=%s preset=%s vsync=%d\n"
            "  dgpu_render=%.2f  transfer=%.2f (memcpy=%.2f)\n"
            "  dgpu_fsr=%.2f  igpu_fsr=%.2f  present=%.2f\n",
            static_cast<unsigned long long>(frameIndex),
            fps(), meanMs(), percentileMs(0.50), percentileMs(0.95), percentileMs(0.99),
            modeName(mode), presetName(preset), vsync ? 1 : 0,
            last.dgpuRenderMs, last.transferTotalMs, last.transferMemcpyMs,
            last.dgpuFsrMs, last.igpuFsrMs, last.presentMs);
    } else {
        std::printf("\rFPS %.1f | %s/%s | cpu %.1fms | render %.1f | xfer %.1f | dFSR %.1f | iFSR %.1f   ",
                    fps(), modeName(mode), presetName(preset), meanMs(),
                    last.dgpuRenderMs, last.transferTotalMs, last.dgpuFsrMs, last.igpuFsrMs);
    }
    std::fflush(stdout);
}

void Bench::dumpCsvLine() {
    std::ofstream f(csvPath, std::ios::app);
    if (!f) {
        std::fprintf(stderr, "Failed to open %s\n", csvPath.c_str());
        return;
    }
    if (!csvHeaderWritten) {
        f.seekp(0, std::ios::end);
        if (f.tellp() == 0) {
            f << "mode,preset,vsync,cpu_frame_ms,fps,"
                 "dgpu_render_ms,transfer_memcpy_ms,transfer_total_ms,"
                 "dgpu_fsr_ms,igpu_fsr_ms,present_ms,"
                 "p50_ms,p95_ms,p99_ms,frames\n";
        }
        csvHeaderWritten = true;
    }
    f << modeName(mode) << ',' << presetName(preset) << ',' << (vsync ? 1 : 0) << ','
      << last.cpuFrameMs << ',' << (last.cpuFrameMs > 0 ? 1000.0 / last.cpuFrameMs : 0) << ','
      << last.dgpuRenderMs << ',' << last.transferMemcpyMs << ',' << last.transferTotalMs << ','
      << last.dgpuFsrMs << ',' << last.igpuFsrMs << ',' << last.presentMs << ','
      << percentileMs(0.50) << ',' << percentileMs(0.95) << ',' << percentileMs(0.99) << ','
      << frameMs.size() << '\n';
    std::printf("\nCSV → %s (cpu=%.2f render=%.2f xfer=%.2f dFsr=%.2f iFsr=%.2f present=%.2f)\n",
                csvPath.c_str(), last.cpuFrameMs, last.dgpuRenderMs, last.transferTotalMs,
                last.dgpuFsrMs, last.igpuFsrMs, last.presentMs);
}

void Bench::resetStats() {
    frameMs.clear();
    frameIndex = 0;
}
