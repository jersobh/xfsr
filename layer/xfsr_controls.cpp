#include "xfsr_controls.hpp"

#include "common.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <dirent.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace xfsr_ctrl {
namespace {

// --- 8×8 ASCII font (32..127), same glyph set as demo HudOverlay ---
static const uint8_t kFont8[96][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00},
    {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},
    {0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00},
    {0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00},
    {0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00},
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00},
    {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E},
    {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00},
    {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F},
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00},
    {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00},
    {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},
    {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

constexpr int kPresetCount = 4;
const char* kPresetNames[kPresetCount] = {"quality", "balanced", "performance", "ultra"};
constexpr float kPresetScales[kPresetCount] = {1.5f, 1.7f, 2.0f, 2.5f};

// X11 keysyms (avoid X11 headers — dlopen only).
constexpr unsigned long kXK_F1 = 0xffbe;
constexpr unsigned long kXK_F2 = 0xffbf;
constexpr unsigned long kXK_F3 = 0xffc0;
constexpr unsigned long kXK_F4 = 0xffc1;
constexpr unsigned long kXK_F5 = 0xffc2;
constexpr unsigned long kXK_F10 = 0xffc7;
constexpr unsigned long kXK_Shift_L = 0xffe1;
constexpr unsigned long kXK_Shift_R = 0xffe2;

struct KeyBits {
    bool f1 = false, f2 = false, f3 = false, f4 = false, f5 = false, f10 = false;
    bool shift = false;
};

struct Runtime {
    std::mutex mu;
    bool inited = false;
    bool hudOn = true;
    bool requireMod = false; // XFSR_HUD_MOD=1 → need Shift+F*
    int presetIdx = 0;
    float scale = 1.5f;
    bool rcas = true;
    bool fsr = true;
    bool offload = false;
    bool oneWay = true;
    bool oneWayForce = false;
    bool overlayPinned = false;
    bool recreate = false;
    bool reconfigureOffload = false;
    bool retryOffload = false;
    std::chrono::steady_clock::time_point toastUntil{};
    char toast[160]{};
    char controlPath[512]{};
    time_t controlMtime = 0;
    bool loggedKeys = false;
};

Runtime g_rt;

using XOpenDisplay_fn = void* (*)(const char*);
using XCloseDisplay_fn = int (*)(void*);
using XQueryKeymap_fn = int (*)(void*, char[32]);
using XKeysymToKeycode_fn = unsigned char (*)(void*, unsigned long);

struct X11 {
    void* lib = nullptr;
    void* dpy = nullptr;
    XOpenDisplay_fn OpenDisplay = nullptr;
    XCloseDisplay_fn CloseDisplay = nullptr;
    XQueryKeymap_fn QueryKeymap = nullptr;
    XKeysymToKeycode_fn KeysymToKeycode = nullptr;
    unsigned char kcF1 = 0, kcF2 = 0, kcF3 = 0, kcF4 = 0, kcF5 = 0, kcF10 = 0;
    unsigned char kcShiftL = 0, kcShiftR = 0;
    bool ready = false;
    bool gaveUp = false;
    int failTries = 0;
};

X11 g_x11;

struct Evdev {
    std::vector<int> fds;
    bool probed = false;
    bool anyOpen = false;
};

Evdev g_evdev;
KeyBits g_prevKeys{};

int presetIndexFromName(const char* p) {
    if (!p) return 0;
    if (std::strcmp(p, "ultra") == 0 || std::strcmp(p, "U") == 0 ||
        std::strcmp(p, "extreme") == 0)
        return 3;
    if (std::strcmp(p, "performance") == 0 || std::strcmp(p, "perf") == 0 ||
        std::strcmp(p, "E") == 0)
        return 2;
    if (std::strcmp(p, "balanced") == 0 || std::strcmp(p, "W") == 0) return 1;
    return 0;
}

int presetIndexFromScale(float s) {
    int best = 0;
    float bestD = 1e9f;
    for (int i = 0; i < kPresetCount; ++i) {
        float d = s - kPresetScales[i];
        if (d < 0) d = -d;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void formatStatusLocked(char* out, size_t n) {
    std::snprintf(out, n,
                  "xfsr %s %.2fx FSR:%s RCAS:%s off:%s 1way:%s [F1-F5 F10]",
                  kPresetNames[g_rt.presetIdx], g_rt.scale, g_rt.fsr ? "on" : "off",
                  g_rt.rcas ? "on" : "off", g_rt.offload ? "on" : "off",
                  g_rt.oneWay ? "on" : "off");
}

void armToastLocked(const char* msg) {
    if (msg && msg[0]) {
        std::snprintf(g_rt.toast, sizeof(g_rt.toast), "%s", msg);
    } else {
        formatStatusLocked(g_rt.toast, sizeof(g_rt.toast));
    }
    g_rt.toastUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::fprintf(stderr, "[xfsr] %s\n", g_rt.toast);
}

void setOffloadLocked(bool on) {
    g_rt.offload = on;
    if (!on) g_rt.oneWay = false;
    setenv("XFSR_OFFLOAD", on ? "1" : "0", 1);
    // Live: same-GPU ↔ round-trip (layer keeps dGPU WSI). One-way still restart-only.
    g_rt.reconfigureOffload = true;
    armToastLocked(on ? "offload on (round-trip)" : "offload off (same-GPU)");
}

void setOneWayLocked(bool on) {
    if (on) g_rt.offload = true; // --one-way implies --offload
    g_rt.oneWay = on;
    setenv("XFSR_OFFLOAD", g_rt.offload ? "1" : "0", 1);
    if (on) {
        setenv("XFSR_IGPU_PRESENT", g_rt.oneWayForce ? "force" : "1", 1);
    } else {
        setenv("XFSR_IGPU_PRESENT", "0", 1);
    }
    // Moving WSI to/from iGPU mid-run SIGSEGVs with MangoHud — preference only.
    g_rt.reconfigureOffload = false;
    if (on)
        armToastLocked(g_rt.oneWayForce ? "one-way on (force) - restart to apply"
                                        : "one-way on - restart to apply");
    else
        armToastLocked("one-way off - restart to apply");
}

void syncGlShimScaleEnvLocked() {
    // Same process as LD_PRELOAD libxfsr_gl_shim — keep XFSR_SCALE in sync so
    // viewport remap tracks F1 (drawable-first path is primary; env is fallback).
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", g_rt.scale);
    setenv("XFSR_SCALE", buf, 1);
    // Drop absolute render hints — they were for launch display size and go stale.
    unsetenv("XFSR_RENDER_W");
    unsetenv("XFSR_RENDER_H");
}

void applyPresetIdxLocked(int idx, bool requestRecreate) {
    if (idx < 0) idx = 0;
    if (idx >= kPresetCount) idx = kPresetCount - 1;
    if (idx == g_rt.presetIdx) return;
    g_rt.presetIdx = idx;
    g_rt.scale = kPresetScales[idx];
    syncGlShimScaleEnvLocked();
    // One-shot OUT_OF_DATE via layer — app/Zink recreates; CreateSwapchain picks up scale.
    if (requestRecreate) g_rt.recreate = true;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "preset %s (%.2fx)", kPresetNames[idx], g_rt.scale);
    armToastLocked(buf);
}

bool ensureX11() {
    if (g_x11.ready) return true;
    if (g_x11.gaveUp) return false;
    if (!std::getenv("DISPLAY")) {
        // Wayland-only: retry a few times in case DISPLAY appears later (Xwayland).
        if (++g_x11.failTries > 120) g_x11.gaveUp = true;
        return false;
    }
    if (!g_x11.lib) {
        g_x11.lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!g_x11.lib) g_x11.lib = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_x11.lib) {
            if (++g_x11.failTries > 30) g_x11.gaveUp = true;
            return false;
        }
        g_x11.OpenDisplay = reinterpret_cast<XOpenDisplay_fn>(dlsym(g_x11.lib, "XOpenDisplay"));
        g_x11.CloseDisplay = reinterpret_cast<XCloseDisplay_fn>(dlsym(g_x11.lib, "XCloseDisplay"));
        g_x11.QueryKeymap = reinterpret_cast<XQueryKeymap_fn>(dlsym(g_x11.lib, "XQueryKeymap"));
        g_x11.KeysymToKeycode =
            reinterpret_cast<XKeysymToKeycode_fn>(dlsym(g_x11.lib, "XKeysymToKeycode"));
        if (!g_x11.OpenDisplay || !g_x11.QueryKeymap || !g_x11.KeysymToKeycode) {
            g_x11.gaveUp = true;
            return false;
        }
    }
    g_x11.dpy = g_x11.OpenDisplay(nullptr);
    if (!g_x11.dpy) {
        if (++g_x11.failTries > 60) g_x11.gaveUp = true;
        return false;
    }
    g_x11.kcF1 = g_x11.KeysymToKeycode(g_x11.dpy, kXK_F1);
    g_x11.kcF2 = g_x11.KeysymToKeycode(g_x11.dpy, kXK_F2);
    g_x11.kcF3 = g_x11.KeysymToKeycode(g_x11.dpy, kXK_F3);
    g_x11.kcF4 = g_x11.KeysymToKeycode(g_x11.dpy, kXK_F4);
    g_x11.kcF5 = g_x11.KeysymToKeycode(g_x11.dpy, kXK_F5);
    g_x11.kcF10 = g_x11.KeysymToKeycode(g_x11.dpy, kXK_F10);
    g_x11.kcShiftL = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Shift_L);
    g_x11.kcShiftR = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Shift_R);
    g_x11.ready = true;
    return true;
}

bool xKeyDown(const char keys[32], unsigned char kc) {
    if (!kc) return false;
    return (keys[kc / 8] & (1 << (kc % 8))) != 0;
}

bool evKeyDown(const unsigned char* bits, int key) {
    if (key < 0) return false;
    const unsigned idx = static_cast<unsigned>(key);
    return (bits[idx / 8] & (1u << (idx % 8))) != 0;
}

void ensureEvdev() {
    if (g_evdev.probed) return;
    g_evdev.probed = true;
    DIR* dir = opendir("/dev/input");
    if (!dir) return;
    while (dirent* ent = readdir(dir)) {
        if (std::strncmp(ent->d_name, "event", 5) != 0) continue;
        char path[256];
        std::snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long evBits = 0;
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), &evBits) < 0 || !(evBits & (1u << EV_KEY))) {
            close(fd);
            continue;
        }
        // Prefer devices that look like keyboards (have KEY_A or KEY_F1).
        unsigned char keyBits[(KEY_MAX + 7) / 8]{};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) < 0) {
            close(fd);
            continue;
        }
        const bool hasF1 = evKeyDown(keyBits, KEY_F1);
        const bool hasA = evKeyDown(keyBits, KEY_A);
        if (!hasF1 && !hasA) {
            close(fd);
            continue;
        }
        g_evdev.fds.push_back(fd);
        g_evdev.anyOpen = true;
    }
    closedir(dir);
}

KeyBits readKeysX11() {
    KeyBits k{};
    if (!ensureX11()) return k;
    char keys[32]{};
    g_x11.QueryKeymap(g_x11.dpy, keys);
    k.f1 = xKeyDown(keys, g_x11.kcF1);
    k.f2 = xKeyDown(keys, g_x11.kcF2);
    k.f3 = xKeyDown(keys, g_x11.kcF3);
    k.f4 = xKeyDown(keys, g_x11.kcF4);
    k.f5 = xKeyDown(keys, g_x11.kcF5);
    k.f10 = xKeyDown(keys, g_x11.kcF10);
    k.shift = xKeyDown(keys, g_x11.kcShiftL) || xKeyDown(keys, g_x11.kcShiftR);
    return k;
}

KeyBits readKeysEvdev() {
    KeyBits k{};
    ensureEvdev();
    if (!g_evdev.anyOpen) return k;
    unsigned char bits[(KEY_MAX + 7) / 8]{};
    for (int fd : g_evdev.fds) {
        unsigned char one[(KEY_MAX + 7) / 8]{};
        if (ioctl(fd, EVIOCGKEY(sizeof(one)), one) < 0) continue;
        for (size_t i = 0; i < sizeof(bits); ++i) bits[i] |= one[i];
    }
    k.f1 = evKeyDown(bits, KEY_F1);
    k.f2 = evKeyDown(bits, KEY_F2);
    k.f3 = evKeyDown(bits, KEY_F3);
    k.f4 = evKeyDown(bits, KEY_F4);
    k.f5 = evKeyDown(bits, KEY_F5);
    k.f10 = evKeyDown(bits, KEY_F10);
    k.shift = evKeyDown(bits, KEY_LEFTSHIFT) || evKeyDown(bits, KEY_RIGHTSHIFT);
    return k;
}

KeyBits mergeKeys(const KeyBits& a, const KeyBits& b) {
    KeyBits k;
    k.f1 = a.f1 || b.f1;
    k.f2 = a.f2 || b.f2;
    k.f3 = a.f3 || b.f3;
    k.f4 = a.f4 || b.f4;
    k.f5 = a.f5 || b.f5;
    k.f10 = a.f10 || b.f10;
    k.shift = a.shift || b.shift;
    return k;
}

void applyHotkeyActions(const KeyBits& cur, const KeyBits& prev, bool modOk) {
    if (!modOk) return;
    auto rise = [](bool now, bool was) { return now && !was; };
    if (rise(cur.f1, prev.f1)) {
        applyPresetIdxLocked((g_rt.presetIdx + 1) % kPresetCount, true);
    }
    if (rise(cur.f2, prev.f2)) {
        g_rt.rcas = !g_rt.rcas;
        armToastLocked(g_rt.rcas ? "RCAS on" : "RCAS off (EASU only)");
    }
    if (rise(cur.f3, prev.f3)) {
        g_rt.fsr = !g_rt.fsr;
        // Layer applies live when FSR pass exists; otherwise next natural recreate.
        armToastLocked(g_rt.fsr ? "FSR on" : "FSR off (blit)");
    }
    if (rise(cur.f4, prev.f4)) {
        setOffloadLocked(!g_rt.offload);
    }
    if (rise(cur.f5, prev.f5)) {
        setOneWayLocked(!g_rt.oneWay);
    }
    if (rise(cur.f10, prev.f10)) {
        g_rt.overlayPinned = !g_rt.overlayPinned;
        armToastLocked(g_rt.overlayPinned ? "overlay pinned" : "overlay unpinned");
    }
}

void pollHotkeysLocked() {
    const KeyBits x11 = readKeysX11();
    const KeyBits ev = readKeysEvdev();
    const KeyBits cur = mergeKeys(x11, ev);
    if (!g_rt.loggedKeys) {
        g_rt.loggedKeys = true;
        std::fprintf(stderr,
                     "[xfsr] hotkeys: x11=%s evdev=%s mod=%s  "
                     "(F1 preset F2 RCAS F3 FSR F4 offload F5 one-way F10 pin)%s\n",
                     g_x11.ready ? "yes" : "no", g_evdev.anyOpen ? "yes" : "no",
                     g_rt.requireMod ? "Shift+" : "none",
                     (!g_x11.ready && !g_evdev.anyOpen)
                         ? " - use control file if keys fail"
                         : "");
    }
    const bool modOk = !g_rt.requireMod || cur.shift;
    applyHotkeyActions(cur, g_prevKeys, modOk);
    g_prevKeys = cur;
}

void trimInPlace(char* s) {
    if (!s) return;
    char* a = s;
    while (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n') ++a;
    if (a != s) std::memmove(s, a, std::strlen(a) + 1);
    size_t n = std::strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

void handleControlLineLocked(char* line) {
    trimInPlace(line);
    if (!line[0] || line[0] == '#') return;
    char cmd[64]{};
    char arg[64]{};
    // Simple "cmd arg" split
    const char* sp = line;
    size_t i = 0;
    while (*sp && *sp != ' ' && *sp != '\t' && i + 1 < sizeof(cmd)) cmd[i++] = *sp++;
    cmd[i] = '\0';
    while (*sp == ' ' || *sp == '\t') ++sp;
    i = 0;
    while (*sp && i + 1 < sizeof(arg)) arg[i++] = *sp++;
    arg[i] = '\0';
    trimInPlace(arg);

    if (std::strcmp(cmd, "preset") == 0) {
        applyPresetIdxLocked(presetIndexFromName(arg), true);
    } else if (std::strcmp(cmd, "cycle_preset") == 0 || std::strcmp(cmd, "cycle") == 0) {
        applyPresetIdxLocked((g_rt.presetIdx + 1) % kPresetCount, true);
    } else if (std::strcmp(cmd, "rcas") == 0) {
        if (arg[0] == '0')
            g_rt.rcas = false;
        else if (arg[0] == '1')
            g_rt.rcas = true;
        else
            g_rt.rcas = !g_rt.rcas;
        armToastLocked(g_rt.rcas ? "RCAS on" : "RCAS off");
    } else if (std::strcmp(cmd, "fsr") == 0) {
        bool next = g_rt.fsr;
        if (arg[0] == '0')
            next = false;
        else if (arg[0] == '1')
            next = true;
        else
            next = !g_rt.fsr;
        g_rt.fsr = next;
        armToastLocked(g_rt.fsr ? "FSR on" : "FSR off (blit)");
    } else if (std::strcmp(cmd, "overlay") == 0 || std::strcmp(cmd, "pin") == 0) {
        if (arg[0] == '0')
            g_rt.overlayPinned = false;
        else if (arg[0] == '1')
            g_rt.overlayPinned = true;
        else
            g_rt.overlayPinned = !g_rt.overlayPinned;
        armToastLocked(g_rt.overlayPinned ? "overlay pinned" : "overlay unpinned");
    } else if (std::strcmp(cmd, "offload") == 0) {
        bool next = g_rt.offload;
        if (arg[0] == '0')
            next = false;
        else if (arg[0] == '1')
            next = true;
        else
            next = !g_rt.offload;
        setOffloadLocked(next);
    } else if (std::strcmp(cmd, "one-way") == 0 || std::strcmp(cmd, "oneway") == 0) {
        bool next = g_rt.oneWay;
        if (arg[0] == '0')
            next = false;
        else if (arg[0] == '1')
            next = true;
        else
            next = !g_rt.oneWay;
        setOneWayLocked(next);
    } else if (std::strcmp(cmd, "status") == 0) {
        armToastLocked(nullptr);
    }
}

void pollControlFileLocked() {
    if (!g_rt.controlPath[0]) return;
    struct stat st {};
    if (stat(g_rt.controlPath, &st) != 0) return;
    if (st.st_mtime == g_rt.controlMtime && st.st_size > 0) {
        // Still process if size changed without mtime granularity — use size+mtime.
    }
    static off_t lastSize = -1;
    if (st.st_mtime == g_rt.controlMtime && st.st_size == lastSize) return;
    g_rt.controlMtime = st.st_mtime;
    lastSize = st.st_size;
    if (st.st_size == 0) return;

    std::ifstream in(g_rt.controlPath);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", line.c_str());
        handleControlLineLocked(buf);
    }
    in.close();
    // Truncate so the next write is a fresh command.
    std::ofstream trunc(g_rt.controlPath, std::ios::trunc);
}

void ensureOverlayGpu(OverlayGpu* ov, DeviceContext* ctx, uint32_t w, uint32_t h) {
    if (!ov || !ctx) return;
    if (ov->ctx == ctx && ov->stagingMapped && ov->width == w && ov->height == h && ov->image.image)
        return;
    destroyOverlay(ov);
    ov->ctx = ctx;
    ov->width = w;
    ov->height = h;
    ctx->loadDispatch();
    auto& d = ctx->vk;
    try {
        ov->image = createImage(ctx, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_TILING_OPTIMAL, false);
    } catch (...) {
        ov->ctx = nullptr;
        return;
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4u;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (d.CreateBuffer(ctx->device, &bci, nullptr, &ov->staging) != VK_SUCCESS) {
        destroyOverlay(ov);
        return;
    }
    VkMemoryRequirements req{};
    d.GetBufferMemoryRequirements(ctx->device, ov->staging, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(ctx->physical, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (d.AllocateMemory(ctx->device, &mai, nullptr, &ov->stagingMem) != VK_SUCCESS) {
        destroyOverlay(ov);
        return;
    }
    d.BindBufferMemory(ctx->device, ov->staging, ov->stagingMem, 0);
    if (d.MapMemory(ctx->device, ov->stagingMem, 0, bytes, 0, &ov->stagingMapped) != VK_SUCCESS) {
        destroyOverlay(ov);
        return;
    }
    ov->lastText[0] = '\0';
}

void rasterizeText(uint8_t* pixels, uint32_t width, uint32_t height, const char* text) {
    const size_t n = static_cast<size_t>(width) * height * 4u;
    std::memset(pixels, 0, n);
    const int panelH = std::min(static_cast<int>(height), 40);
    for (int y = 0; y < panelH; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* p = pixels + (static_cast<size_t>(y) * width + x) * 4;
            p[0] = 10;
            p[1] = 12;
            p[2] = 18;
            p[3] = 210;
        }
    }
    int x = 8;
    int y = 10;
    const int scale = 2;
    for (const char* t = text; *t; ++t) {
        char c = *t;
        if (c == '\n') {
            x = 8;
            y += 8 * scale + 4;
            continue;
        }
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc > 127) c = '?';
        const uint8_t* glyph = kFont8[static_cast<unsigned char>(c) - 32];
        for (int row = 0; row < 8; ++row) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; ++col) {
                if (!(bits & (1u << col))) continue;
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;
                        if (px < 0 || py < 0 || px >= static_cast<int>(width) ||
                            py >= static_cast<int>(height))
                            continue;
                        uint8_t* p =
                            pixels + (static_cast<size_t>(py) * width + static_cast<size_t>(px)) * 4;
                        p[0] = 90;
                        p[1] = 255;
                        p[2] = 160;
                        p[3] = 255;
                    }
                }
            }
        }
        x += 8 * scale + 2;
        if (x + 8 * scale >= static_cast<int>(width)) {
            x = 8;
            y += 8 * scale + 4;
        }
    }
}

void transition(DeviceContext* ctx, VkCommandBuffer cmd, VkImage image, VkImageLayout oldL,
                VkImageLayout newL, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    ctx->vk.CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

void initFromEnv() {
    std::lock_guard<std::mutex> lock(g_rt.mu);
    if (g_rt.inited) return;
    g_rt.inited = true;

    const char* hud = std::getenv("XFSR_HUD");
    if (!hud) hud = std::getenv("CROSSVULKAN_HUD");
    // Default ON; XFSR_HUD=0 disables.
    g_rt.hudOn = !(hud && hud[0] == '0');

    // Default: bare F-keys (no modifier). XFSR_HUD_MOD=1 requires Shift+F*.
    const char* mod = std::getenv("XFSR_HUD_MOD");
    g_rt.requireMod = (mod && mod[0] != '\0' && mod[0] != '0');

    const char* preset = std::getenv("XFSR_PRESET");
    if (!preset || !preset[0]) preset = std::getenv("CROSSVULKAN_PRESET_MODE");
    g_rt.presetIdx = presetIndexFromName(preset);

    const char* scaleEnv = std::getenv("XFSR_SCALE");
    if (scaleEnv && scaleEnv[0]) {
        // Minimal float parse (no strtof — Proton glibc).
        float v = 0.f, frac = 0.f, place = 0.1f;
        const char* p = scaleEnv;
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
        v += frac;
        if (v >= 1.01f && v <= 8.f) {
            g_rt.scale = v;
            g_rt.presetIdx = presetIndexFromScale(v);
        } else {
            g_rt.scale = kPresetScales[g_rt.presetIdx];
        }
    } else {
        g_rt.scale = kPresetScales[g_rt.presetIdx];
    }

    const char* rcas = std::getenv("XFSR_RCAS");
    if (!rcas) rcas = std::getenv("CROSSVULKAN_RCAS");
    g_rt.rcas = !(rcas && rcas[0] == '0');

    const char* fsr = std::getenv("XFSR_LAYER_FSR");
    if (!fsr || !fsr[0]) fsr = std::getenv("XFSR_FSR");
    if (!fsr || !fsr[0]) fsr = std::getenv("CROSSVULKAN_FSR");
    g_rt.fsr = !(fsr && fsr[0] == '0');

    const char* off = std::getenv("XFSR_OFFLOAD");
    if (!off) off = std::getenv("CROSSVULKAN_OFFLOAD");
    // Explicit XFSR_OFFLOAD=0 wins — never let IGPU_PRESENT=force turn offload back on.
    const bool offloadExplicitOff = off && off[0] == '0';
    g_rt.offload = (off && off[0] && off[0] != '0');

    const char* ow = std::getenv("XFSR_IGPU_PRESENT");
    if (!ow) ow = std::getenv("CROSSVULKAN_IGPU_PRESENT");
    g_rt.oneWayForce = false;
    if (ow && (std::strcmp(ow, "force") == 0 || ow[0] == '2')) {
        g_rt.oneWay = true;
        g_rt.oneWayForce = true;
        if (!offloadExplicitOff) g_rt.offload = true;
    } else if (ow && ow[0] == '0') {
        g_rt.oneWay = false;
    } else if (ow && ow[0] == '1') {
        g_rt.oneWay = true;
        if (!offloadExplicitOff) g_rt.offload = true;
    } else {
        // Layer default: prefer one-way when offload is on (NVIDIA still gated).
        g_rt.oneWay = g_rt.offload;
    }

    const char* cpath = std::getenv("XFSR_CONTROL");
    if (cpath && cpath[0]) {
        std::snprintf(g_rt.controlPath, sizeof(g_rt.controlPath), "%s", cpath);
    } else {
        const char* rt = std::getenv("XDG_RUNTIME_DIR");
        if (rt && rt[0])
            std::snprintf(g_rt.controlPath, sizeof(g_rt.controlPath), "%s/xfsr-control", rt);
        else
            std::snprintf(g_rt.controlPath, sizeof(g_rt.controlPath), "/tmp/xfsr-control-%d",
                          static_cast<int>(getpid()));
    }

    if (g_rt.hudOn) {
        std::fprintf(stderr,
                     "[xfsr] controls on: F1 preset F2 RCAS F3 FSR F4 offload F5 one-way F10 pin%s\n"
                     "[xfsr] control file: %s  (preset|rcas|fsr|offload|one-way|overlay|status)\n",
                     g_rt.requireMod ? " (hold Shift)" : "", g_rt.controlPath);
        // Touch the control file so the path always exists for scripting.
        std::ofstream touch(g_rt.controlPath, std::ios::app);
    }
}

bool enabled() {
    initFromEnv();
    std::lock_guard<std::mutex> lock(g_rt.mu);
    return g_rt.hudOn;
}

Snapshot poll() {
    initFromEnv();
    Snapshot s{};
    std::lock_guard<std::mutex> lock(g_rt.mu);
    if (!g_rt.hudOn) {
        s.scale = g_rt.scale;
        s.preset = kPresetNames[g_rt.presetIdx];
        s.rcas = g_rt.rcas;
        s.fsr = g_rt.fsr;
        s.offload = g_rt.offload;
        s.oneWay = g_rt.oneWay;
        s.oneWayForce = g_rt.oneWayForce;
        formatStatusLocked(s.status, sizeof(s.status));
        return s;
    }
    pollHotkeysLocked();
    pollControlFileLocked();

    s.scale = g_rt.scale;
    s.preset = kPresetNames[g_rt.presetIdx];
    s.rcas = g_rt.rcas;
    s.fsr = g_rt.fsr;
    s.offload = g_rt.offload;
    s.oneWay = g_rt.oneWay;
    s.oneWayForce = g_rt.oneWayForce;
    s.overlayPinned = g_rt.overlayPinned;
    s.recreateSwapchain = g_rt.recreate;
    s.reconfigureOffload = g_rt.reconfigureOffload;
    s.retryOffload = g_rt.retryOffload;
    const bool toastLive = std::chrono::steady_clock::now() < g_rt.toastUntil;
    s.showOverlay = g_rt.overlayPinned || toastLive;
    if (g_rt.overlayPinned)
        formatStatusLocked(s.status, sizeof(s.status));
    else if (toastLive)
        std::snprintf(s.status, sizeof(s.status), "%s", g_rt.toast);
    else
        s.status[0] = '\0';
    return s;
}

Snapshot peek() {
    initFromEnv();
    Snapshot s{};
    std::lock_guard<std::mutex> lock(g_rt.mu);
    s.scale = g_rt.scale;
    s.preset = kPresetNames[g_rt.presetIdx];
    s.rcas = g_rt.rcas;
    s.fsr = g_rt.fsr;
    s.offload = g_rt.offload;
    s.oneWay = g_rt.oneWay;
    s.oneWayForce = g_rt.oneWayForce;
    s.overlayPinned = g_rt.overlayPinned;
    s.recreateSwapchain = g_rt.recreate;
    s.reconfigureOffload = g_rt.reconfigureOffload;
    s.retryOffload = g_rt.retryOffload;
    formatStatusLocked(s.status, sizeof(s.status));
    return s;
}

void clearRecreateRequest() {
    std::lock_guard<std::mutex> lock(g_rt.mu);
    g_rt.recreate = false;
}

void clearReconfigureOffload() {
    std::lock_guard<std::mutex> lock(g_rt.mu);
    g_rt.reconfigureOffload = false;
}

void clearRetryOffload() {
    std::lock_guard<std::mutex> lock(g_rt.mu);
    g_rt.retryOffload = false;
}

void showToast(const char* msg) {
    initFromEnv();
    std::lock_guard<std::mutex> lock(g_rt.mu);
    armToastLocked(msg);
}

void destroyOverlay(OverlayGpu* ov) {
    if (!ov) return;
    if (ov->ctx && ov->ctx->device) {
        ov->ctx->loadDispatch();
        auto& d = ov->ctx->vk;
        if (ov->stagingMapped && d.UnmapMemory) {
            d.UnmapMemory(ov->ctx->device, ov->stagingMem);
            ov->stagingMapped = nullptr;
        }
        if (ov->staging && d.DestroyBuffer) d.DestroyBuffer(ov->ctx->device, ov->staging, nullptr);
        if (ov->stagingMem && d.FreeMemory) d.FreeMemory(ov->ctx->device, ov->stagingMem, nullptr);
    }
    ov->image.destroy();
    ov->staging = VK_NULL_HANDLE;
    ov->stagingMem = VK_NULL_HANDLE;
    ov->stagingMapped = nullptr;
    ov->width = ov->height = 0;
    ov->lastText[0] = '\0';
    ov->ctx = nullptr;
}

bool recordOverlay(OverlayGpu* ov, DeviceContext* ctx, VkCommandBuffer cmd, VkImage swapImage,
                   uint32_t swapW, uint32_t swapH, const char* text) {
    if (!ov || !ctx || !cmd || !swapImage || !text || !text[0]) return false;
    if (swapW < 64 || swapH < 32) return false;

    constexpr uint32_t kOw = 900;
    constexpr uint32_t kOh = 40;
    ensureOverlayGpu(ov, ctx, kOw, kOh);
    if (!ov->stagingMapped || !ov->image.image) return false;

    ctx->loadDispatch();
    auto& d = ctx->vk;
    if (!d.CmdCopyBufferToImage || !d.CmdBlitImage) return false;

    if (std::strncmp(ov->lastText, text, sizeof(ov->lastText)) != 0) {
        std::vector<uint8_t> pixels(static_cast<size_t>(ov->width) * ov->height * 4u);
        rasterizeText(pixels.data(), ov->width, ov->height, text);
        std::memcpy(ov->stagingMapped, pixels.data(), pixels.size());
        std::snprintf(ov->lastText, sizeof(ov->lastText), "%s", text);
    }

    transition(ctx, cmd, ov->image.image, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {ov->width, ov->height, 1};
    d.CmdCopyBufferToImage(cmd, ov->staging, ov->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &copy);

    transition(ctx, cmd, ov->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_TRANSFER_READ_BIT);

    // Bottom-left — top is usually MangoHud.
    const int32_t margin = 12;
    const int32_t dw =
        static_cast<int32_t>(std::min(ov->width, swapW > 24u ? swapW - 24u : ov->width));
    const int32_t dh =
        static_cast<int32_t>(std::min(ov->height, swapH > 24u ? swapH - 24u : ov->height));
    const int32_t dstY0 = std::max(0, static_cast<int32_t>(swapH) - dh - margin);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {static_cast<int32_t>(ov->width), static_cast<int32_t>(ov->height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {margin, dstY0, 0};
    blit.dstOffsets[1] = {margin + dw, dstY0 + dh, 1};
    d.CmdBlitImage(cmd, ov->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    return true;
}

} // namespace xfsr_ctrl
