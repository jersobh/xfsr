#pragma once

#include "common.hpp"

#include <deque>
#include <fstream>
#include <string>

struct BenchSample {
    double cpuFrameMs = 0;
    double dgpuRenderMs = 0;
    double transferMemcpyMs = 0;
    double transferTotalMs = 0;
    double dgpuFsrMs = 0;
    double igpuFsrMs = 0;
    double presentMs = 0;
};

struct Bench {
    static constexpr size_t kWindow = 1000;

    std::deque<double> frameMs;
    BenchSample last{};
    BenchMode mode = BenchMode::Native;
    FsrPreset preset = FsrPreset::Quality;
    bool vsync = false;
    uint64_t frameIndex = 0;
    std::string csvPath = "bench.csv";
    bool csvHeaderWritten = false;

    void push(const BenchSample& s);
    double meanMs() const;
    double percentileMs(double p) const; // p in [0,1]
    double fps() const;

    std::string formatOverlay() const;   // multi-line text for on-screen HUD
    std::string formatTitle() const;     // short FPS for window title
    void printHud(bool forceNewline = false) const;
    void dumpCsvLine();
    void resetStats();
};

void renderSizeForPreset(FsrPreset preset, uint32_t displayW, uint32_t displayH,
                         uint32_t& outW, uint32_t& outH);
