#pragma once

#include <volk.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>

inline void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s failed: VkResult=%d", what, static_cast<int>(r));
        throw std::runtime_error(buf);
    }
}

inline uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    throw std::runtime_error("No suitable memory type");
}

// Prefer HOST_CACHED for CPU readback (NVIDIA WC/uncached is ~100 MB/s; cached is ~GB/s).
inline uint32_t findMemoryTypePrefer(VkPhysicalDevice phys, uint32_t typeBits,
                                     VkMemoryPropertyFlags required,
                                     VkMemoryPropertyFlags preferred) {
    try {
        return findMemoryType(phys, typeBits, required | preferred);
    } catch (...) {
        return findMemoryType(phys, typeBits, required);
    }
}

inline VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) {
    return (v + a - 1) & ~(a - 1);
}

inline std::vector<uint32_t> readSpirv(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("Failed to open shader: " + path);
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0 || (size % 4) != 0) {
        std::fclose(f);
        throw std::runtime_error("Invalid SPIR-V size: " + path);
    }
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    if (std::fread(code.data(), 1, static_cast<size_t>(size), f) != static_cast<size_t>(size)) {
        std::fclose(f);
        throw std::runtime_error("Failed to read shader: " + path);
    }
    std::fclose(f);
    return code;
}

inline VkShaderModule createShaderModule(VkDevice device, const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size() * sizeof(uint32_t);
    ci.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(device, &ci, nullptr, &mod), "vkCreateShaderModule");
    return mod;
}

enum class BenchMode : int {
    Native = 0,
    FsrLocal = 1,       // FSR 1.0 spatial on dGPU
    FsrOffload = 2,     // FSR 1.0 spatial on iGPU
    Fsr3Local = 3,      // FSR 3.1 temporal on dGPU
    Fsr3Offload = 4,    // FSR 3.1 temporal on iGPU (color+depth+MV transfer)
};

enum class FsrPreset : int {
    Quality = 0,    // 1.5x
    Balanced = 1,   // 1.7x
    Performance = 2 // 2.0x
};

inline float presetScale(FsrPreset p) {
    switch (p) {
    case FsrPreset::Quality: return 1.5f;
    case FsrPreset::Balanced: return 1.7f;
    case FsrPreset::Performance: return 2.0f;
    }
    return 1.5f;
}

inline const char* modeName(BenchMode m) {
    switch (m) {
    case BenchMode::Native: return "native";
    case BenchMode::FsrLocal: return "fsr1_local";
    case BenchMode::FsrOffload: return "fsr1_offload";
    case BenchMode::Fsr3Local: return "fsr3_local";
    case BenchMode::Fsr3Offload: return "fsr3_offload";
    }
    return "?";
}

inline const char* presetName(FsrPreset p) {
    switch (p) {
    case FsrPreset::Quality: return "quality";
    case FsrPreset::Balanced: return "balanced";
    case FsrPreset::Performance: return "performance";
    }
    return "?";
}

// Prefer XFSR_*; accept legacy CROSSVULKAN_* for one release.
inline const char* xfsrEnv(const char* name, const char* legacy = nullptr) {
    if (const char* e = std::getenv(name); e && e[0]) return e;
    if (legacy) {
        if (const char* e = std::getenv(legacy); e && e[0]) return e;
    }
    return nullptr;
}

// Env: XFSR_FSR=1|3 (default 1), XFSR_PRESET=quality|balanced|performance,
//      XFSR_OFFLOAD=0|1 (default 1 → FSR on iGPU).
inline BenchMode modeFromEnv() {
    const char* fsr = xfsrEnv("XFSR_FSR", "CROSSVULKAN_FSR");
    const char* off = xfsrEnv("XFSR_OFFLOAD", "CROSSVULKAN_OFFLOAD");
    int ver = 1;
    if (fsr && fsr[0]) ver = std::atoi(fsr);
    bool offload = true;
    if (off && off[0]) offload = (std::atoi(off) != 0);
    if (ver == 3) return offload ? BenchMode::Fsr3Offload : BenchMode::Fsr3Local;
    if (ver == 0) return BenchMode::Native;
    return offload ? BenchMode::FsrOffload : BenchMode::FsrLocal;
}

inline FsrPreset presetFromEnv() {
    const char* p = xfsrEnv("XFSR_PRESET", "CROSSVULKAN_PRESET_MODE");
    if (!p || !p[0]) return FsrPreset::Quality;
    if (std::strcmp(p, "balanced") == 0 || std::strcmp(p, "w") == 0) return FsrPreset::Balanced;
    if (std::strcmp(p, "performance") == 0 || std::strcmp(p, "e") == 0 ||
        std::strcmp(p, "perf") == 0) {
        return FsrPreset::Performance;
    }
    return FsrPreset::Quality;
}

struct FrameTimings {
    double renderMs = 0;
    double transferMs = 0;
    double fsrMs = 0;
    double presentMs = 0;
    double cpuFrameMs = 0;
};
