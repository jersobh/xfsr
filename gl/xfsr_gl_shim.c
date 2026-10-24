/* LD_PRELOAD shim for OpenGL-via-Zink + xfsr FSR.
 *
 * GL apps call glViewport(0,0, windowW, windowH) while caps-shrink made the
 * swapchain windowW/scale × windowH/scale. Remap full-window viewports/scissors.
 *
 * Must export interpose symbols (visibility default) — CMake builds with
 * -fvisibility=hidden. Also wrap glX/egl/glfw GetProcAddress so apps that
 * resolve GL via those (not the PLT) still get remapped viewports.
 *
 * Env (xfsr-run):
 *   XFSR_SCALE              — FSR scale (1.5 / 1.7 / 2.0)  [preferred]
 *   XFSR_RENDER_W/H         — optional absolute render size
 *   XFSR_DISPLAY_W/H        — optional absolute window size
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XFSR_EXPORT __attribute__((visibility("default")))

typedef int GLint;
typedef int GLsizei;

typedef void (*GlViewportFn)(GLint, GLint, GLsizei, GLsizei);
typedef void (*GlScissorFn)(GLint, GLint, GLsizei, GLsizei);
typedef void (*GlViewportArrayvFn)(unsigned int, GLsizei, const float*);
typedef void (*GlScissorArrayvFn)(unsigned int, GLsizei, const GLint*);
typedef void (*GlViewportIndexedfFn)(unsigned int, float, float, float, float);
typedef void (*GlScissorIndexedFn)(unsigned int, GLint, GLint, GLsizei, GLsizei);
typedef void* (*GetProcFn)(const char*);

static int g_dw, g_dh, g_rw, g_rh;
static float g_scale;
static int g_ready;
static int g_logged;
/* Last real glfw framebuffer size (window pixels), before we lie render size. */
static int g_fb_w;
static int g_fb_h;

static GlViewportFn real_glViewport;
static GlScissorFn real_glScissor;
static GlViewportArrayvFn real_glViewportArrayv;
static GlScissorArrayvFn real_glScissorArrayv;
static GlViewportIndexedfFn real_glViewportIndexedf;
static GlScissorIndexedFn real_glScissorIndexed;

/* Forward decls — wrap_proc / dlsym return these. */
XFSR_EXPORT void* glXGetProcAddress(const char* name);
XFSR_EXPORT void* glXGetProcAddressARB(const char* name);
XFSR_EXPORT void* eglGetProcAddress(const char* name);
XFSR_EXPORT void* glfwGetProcAddress(const char* name);
XFSR_EXPORT void* dlsym(void* handle, const char* symbol);
static void* resolve_next(const char* name, void* self);
static int query_egl_drawable(int* out_w, int* out_h);

static void load_extents(void) {
    if (g_ready) return;
    const char* dw = getenv("XFSR_DISPLAY_W");
    const char* dh = getenv("XFSR_DISPLAY_H");
    const char* rw = getenv("XFSR_RENDER_W");
    const char* rh = getenv("XFSR_RENDER_H");
    const char* sc = getenv("XFSR_SCALE");
    g_dw = dw ? atoi(dw) : 0;
    g_dh = dh ? atoi(dh) : 0;
    g_rw = rw ? atoi(rw) : 0;
    g_rh = rh ? atoi(rh) : 0;
    g_scale = sc ? strtof(sc, NULL) : 0.f;
    if (g_scale < 1.01f) g_scale = 0.f;
    g_ready = 1;
    if (getenv("XFSR_VERBOSE")) {
        fprintf(stderr, "[xfsr-gl] shim scale=%.2f render=%dx%d display_hint=%dx%d\n",
                g_scale, g_rw, g_rh, g_dw, g_dh);
    }
}

/* Map a window/fb size to the Zink color-image (render) size.
 * Fullscreen matching XFSR_OUTPUT_* → absolute XFSR_RENDER_*.
 * Windowed (glmark2 800x600) → window/scale — never force 1080p's 768x432. */
static void target_render_size(int win_w, int win_h, int* out_rw, int* out_rh) {
    *out_rw = 0;
    *out_rh = 0;
    if (win_w < 64 || win_h < 64) return;
    const int near = 2;
    if (g_dw > 0 && g_dh > 0 && g_rw > 0 && g_rh > 0 && abs(win_w - g_dw) <= near &&
        abs(win_h - g_dh) <= near) {
        *out_rw = g_rw;
        *out_rh = g_rh;
        return;
    }
    if (g_scale >= 1.01f) {
        int rw = (int)(win_w / g_scale + 0.5f);
        int rh = (int)(win_h / g_scale + 0.5f);
        if (rw < 64) rw = 64;
        if (rh < 64) rh = 64;
        *out_rw = rw;
        *out_rh = rh;
    }
}

/* EGL drawable size = Zink color image. If it still equals the window, the layer
 * took the identity path (no caps-shrink) — remapping would postage-stamp.
 * Only remap when the drawable is already at render size. */
static int query_egl_drawable(int* out_w, int* out_h) {
    typedef void* (*EglGetCurrentDisplayFn)(void);
    typedef void* (*EglGetCurrentSurfaceFn)(unsigned int);
    typedef unsigned int (*EglQuerySurfaceFn)(void*, void*, int, int*);
    enum { EGL_DRAW = 0x3059, EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056 };
    static EglGetCurrentDisplayFn get_dpy;
    static EglGetCurrentSurfaceFn get_surf;
    static EglQuerySurfaceFn query;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        get_dpy = (EglGetCurrentDisplayFn)resolve_next("eglGetCurrentDisplay", NULL);
        get_surf = (EglGetCurrentSurfaceFn)resolve_next("eglGetCurrentSurface", NULL);
        query = (EglQuerySurfaceFn)resolve_next("eglQuerySurface", NULL);
    }
    if (!get_dpy || !get_surf || !query) return 0;
    void* dpy = get_dpy();
    void* surf = get_surf(EGL_DRAW);
    if (!dpy || !surf) return 0;
    int w = 0, h = 0;
    if (!query(dpy, surf, EGL_WIDTH, &w) || !query(dpy, surf, EGL_HEIGHT, &h)) return 0;
    if (w < 64 || h < 64) return 0;
    *out_w = w;
    *out_h = h;
    return 1;
}

static void remap_rect(GLint* x, GLint* y, GLsizei* w, GLsizei* h) {
    load_extents();
    if (!w || !h) return;
    const GLint ox = x ? *x : 0;
    const GLint oy = y ? *y : 0;
    const GLsizei ow = *w, oh = *h;

    if (ox != 0 || oy != 0 || *w <= 64 || *h <= 64) return;

    int tw = 0, th = 0;
    /* Prefer real window pixels as the source size for scaling. */
    const int win_w = (g_fb_w > 64) ? g_fb_w : (int)*w;
    const int win_h = (g_fb_h > 64) ? g_fb_h : (int)*h;
    target_render_size(win_w, win_h, &tw, &th);
    if (tw <= 0 || th <= 0) return;

    /* Already at target render size — never shrink twice. */
    if (abs((int)*w - tw) <= 1 && abs((int)*h - th) <= 1) return;

    /* Only remap full-window viewports (matches window or display hint). */
    const int near = 2;
    const int is_window =
        (abs((int)*w - win_w) <= near && abs((int)*h - win_h) <= near);
    const int is_display =
        (g_dw > 0 && g_dh > 0 && abs((int)*w - g_dw) <= near && abs((int)*h - g_dh) <= near);
    if (!is_window && !is_display) return;

    /* Wayland identity path: drawable still window-sized → do not shrink viewport. */
    int dw = 0, dh = 0;
    if (query_egl_drawable(&dw, &dh)) {
        if (abs(dw - win_w) <= near && abs(dh - win_h) <= near) return; /* identity */
        /* Caps-honored: drawable ≈ render size — remap window viewport into it. */
        if (abs(dw - tw) > 16 || abs(dh - th) > 16) {
            tw = dw;
            th = dh;
        }
    }

    if (x) *x = 0;
    if (y) *y = 0;
    *w = tw;
    *h = th;

    if (!g_logged && (*w != ow || *h != oh) && getenv("XFSR_VERBOSE")) {
        g_logged = 1;
        fprintf(stderr, "[xfsr-gl] remap %dx%d → %dx%d (glfw_fb=%dx%d win=%dx%d egl=%dx%d)\n",
                (int)ow, (int)oh, (int)*w, (int)*h, g_fb_w, g_fb_h, win_w, win_h, dw, dh);
    }
}

typedef void* (*DlsymFn)(void*, const char*);
typedef void* (*DlvsymFn)(void*, const char*, const char*);
static DlsymFn real_dlsym;
static void ensure_dlsym(void);

/* libc/next dlsym — never our interposed symbol (same-DSO bind would recurse). */
static void* libc_dlsym(void* handle, const char* name) {
    ensure_dlsym();
    if (!real_dlsym) return NULL;
    return real_dlsym(handle, name);
}

static void* resolve_next(const char* name, void* self) {
    /* Prefer the next LD_PRELOAD (MangoHud_shim) so SwapBuffers / GetProc stay chained. */
    void* p = libc_dlsym(RTLD_NEXT, name);
    if (p && p != self) return p;
    void* gl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (!gl) gl = dlopen("libGL.so.1", RTLD_LAZY);
    if (gl) {
        p = libc_dlsym(gl, name);
        if (p && p != self) return p;
    }
    void* glx = dlopen("libGLX.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!glx) glx = dlopen("libGLX.so.0", RTLD_LAZY);
    if (glx) {
        p = libc_dlsym(glx, name);
        if (p && p != self) return p;
    }
    void* egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (!egl) egl = dlopen("libEGL.so.1", RTLD_LAZY);
    if (egl) {
        p = libc_dlsym(egl, name);
        if (p && p != self) return p;
    }
    return NULL;
}

static void stash_if_real(void** slot, void* candidate, void* wrapper) {
    if (!candidate || candidate == wrapper) return;
    if (!*slot) *slot = candidate;
}

XFSR_EXPORT void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (!real_glViewport)
        real_glViewport = (GlViewportFn)resolve_next("glViewport", (void*)glViewport);
    if (!real_glViewport) return;
    remap_rect(&x, &y, &width, &height);
    real_glViewport(x, y, width, height);
}

XFSR_EXPORT void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (!real_glScissor)
        real_glScissor = (GlScissorFn)resolve_next("glScissor", (void*)glScissor);
    if (!real_glScissor) return;
    remap_rect(&x, &y, &width, &height);
    real_glScissor(x, y, width, height);
}

XFSR_EXPORT void glViewportIndexedf(unsigned int index, float x, float y, float w, float h) {
    if (!real_glViewportIndexedf)
        real_glViewportIndexedf =
            (GlViewportIndexedfFn)resolve_next("glViewportIndexedf", (void*)glViewportIndexedf);
    if (!real_glViewportIndexedf) return;
    if (index == 0) {
        GLint ix = (GLint)x, iy = (GLint)y;
        GLsizei iw = (GLsizei)(w + 0.5f), ih = (GLsizei)(h + 0.5f);
        remap_rect(&ix, &iy, &iw, &ih);
        real_glViewportIndexedf(index, (float)ix, (float)iy, (float)iw, (float)ih);
        return;
    }
    real_glViewportIndexedf(index, x, y, w, h);
}

XFSR_EXPORT void glScissorIndexed(unsigned int index, GLint x, GLint y, GLsizei w, GLsizei h) {
    if (!real_glScissorIndexed)
        real_glScissorIndexed =
            (GlScissorIndexedFn)resolve_next("glScissorIndexed", (void*)glScissorIndexed);
    if (!real_glScissorIndexed) return;
    if (index == 0) remap_rect(&x, &y, &w, &h);
    real_glScissorIndexed(index, x, y, w, h);
}

XFSR_EXPORT void glViewportArrayv(unsigned int first, GLsizei count, const float* v) {
    if (!real_glViewportArrayv)
        real_glViewportArrayv =
            (GlViewportArrayvFn)resolve_next("glViewportArrayv", (void*)glViewportArrayv);
    if (!real_glViewportArrayv || !v || count <= 0) {
        if (real_glViewportArrayv) real_glViewportArrayv(first, count, v);
        return;
    }
    if (first == 0) {
        float tmp[4];
        memcpy(tmp, v, sizeof(tmp));
        GLint x = (GLint)tmp[0], y = (GLint)tmp[1];
        GLsizei w = (GLsizei)(tmp[2] + 0.5f), h = (GLsizei)(tmp[3] + 0.5f);
        remap_rect(&x, &y, &w, &h);
        tmp[0] = (float)x;
        tmp[1] = (float)y;
        tmp[2] = (float)w;
        tmp[3] = (float)h;
        if (count == 1) {
            real_glViewportArrayv(first, 1, tmp);
            return;
        }
        float* buf = (float*)malloc(sizeof(float) * 4 * (size_t)count);
        if (!buf) {
            real_glViewportArrayv(first, count, v);
            return;
        }
        memcpy(buf, v, sizeof(float) * 4 * (size_t)count);
        memcpy(buf, tmp, sizeof(tmp));
        real_glViewportArrayv(first, count, buf);
        free(buf);
        return;
    }
    real_glViewportArrayv(first, count, v);
}

XFSR_EXPORT void glScissorArrayv(unsigned int first, GLsizei count, const GLint* v) {
    if (!real_glScissorArrayv)
        real_glScissorArrayv =
            (GlScissorArrayvFn)resolve_next("glScissorArrayv", (void*)glScissorArrayv);
    if (!real_glScissorArrayv || !v || count <= 0) {
        if (real_glScissorArrayv) real_glScissorArrayv(first, count, v);
        return;
    }
    if (first == 0) {
        GLint tmp[4];
        memcpy(tmp, v, sizeof(tmp));
        remap_rect(&tmp[0], &tmp[1], &tmp[2], &tmp[3]);
        if (count == 1) {
            real_glScissorArrayv(first, 1, tmp);
            return;
        }
        GLint* buf = (GLint*)malloc(sizeof(GLint) * 4 * (size_t)count);
        if (!buf) {
            real_glScissorArrayv(first, count, v);
            return;
        }
        memcpy(buf, v, sizeof(GLint) * 4 * (size_t)count);
        memcpy(buf, tmp, sizeof(tmp));
        real_glScissorArrayv(first, count, buf);
        free(buf);
        return;
    }
    real_glScissorArrayv(first, count, v);
}

XFSR_EXPORT void glfwGetFramebufferSize(void* window, int* width, int* height) {
    typedef void (*Fn)(void*, int*, int*);
    static Fn real = NULL;
    if (!real) real = (Fn)resolve_next("glfwGetFramebufferSize", (void*)glfwGetFramebufferSize);
    if (!real) return;
    real(window, width, height);
    if (!width || !height) return;
    g_fb_w = *width;
    g_fb_h = *height;
    load_extents();
    /* Lie render size only when Zink's drawable is already caps-shrunk. On
     * Wayland identity (drawable == window) lying would desync projection. */
    int dw = 0, dh = 0;
    if (!query_egl_drawable(&dw, &dh)) return;
    int tw = 0, th = 0;
    target_render_size(g_fb_w, g_fb_h, &tw, &th);
    if (tw <= 0 || th <= 0) return;
    if (abs(dw - tw) <= 2 && abs(dh - th) <= 2 && (tw != *width || th != *height)) {
        *width = tw;
        *height = th;
    }
}

XFSR_EXPORT void glfwGetWindowSize(void* window, int* width, int* height) {
    typedef void (*Fn)(void*, int*, int*);
    static Fn real = NULL;
    if (!real) real = (Fn)resolve_next("glfwGetWindowSize", (void*)glfwGetWindowSize);
    if (!real) return;
    real(window, width, height);
    /* Keep window size as the real window — only framebuffer size is remapped. */
}

static void* wrap_proc(const char* name, void* real) {
    if (!name) return real;
    /* glmark2 dlopens libGL and dlsyms these by name — LD_PRELOAD alone never sees them. */
    if (strcmp(name, "glXGetProcAddress") == 0) return (void*)glXGetProcAddress;
    if (strcmp(name, "glXGetProcAddressARB") == 0) return (void*)glXGetProcAddressARB;
    if (strcmp(name, "eglGetProcAddress") == 0) return (void*)eglGetProcAddress;
    if (strcmp(name, "glfwGetProcAddress") == 0) return (void*)glfwGetProcAddress;
    if (!real) return real;
    if (strcmp(name, "glViewport") == 0) {
        stash_if_real((void**)&real_glViewport, real, (void*)glViewport);
        return (void*)glViewport;
    }
    if (strcmp(name, "glScissor") == 0) {
        stash_if_real((void**)&real_glScissor, real, (void*)glScissor);
        return (void*)glScissor;
    }
    if (strcmp(name, "glViewportIndexedf") == 0) {
        stash_if_real((void**)&real_glViewportIndexedf, real, (void*)glViewportIndexedf);
        return (void*)glViewportIndexedf;
    }
    if (strcmp(name, "glScissorIndexed") == 0) {
        stash_if_real((void**)&real_glScissorIndexed, real, (void*)glScissorIndexed);
        return (void*)glScissorIndexed;
    }
    if (strcmp(name, "glViewportArrayv") == 0) {
        stash_if_real((void**)&real_glViewportArrayv, real, (void*)glViewportArrayv);
        return (void*)glViewportArrayv;
    }
    if (strcmp(name, "glScissorArrayv") == 0) {
        stash_if_real((void**)&real_glScissorArrayv, real, (void*)glScissorArrayv);
        return (void*)glScissorArrayv;
    }
    return real;
}

static void ensure_dlsym(void) {
    if (real_dlsym) return;
    /* Do NOT call dlsym() here — we define it (same-DSO bind → recursion).
     * libc dlvsym is not interposed, so RTLD_NEXT finds the real dlsym. */
    DlvsymFn real_dlvsym = (DlvsymFn)dlvsym(RTLD_NEXT, "dlvsym", "GLIBC_2.34");
    if (!real_dlvsym) real_dlvsym = (DlvsymFn)dlvsym(RTLD_NEXT, "dlvsym", "GLIBC_2.2.5");
    if (real_dlvsym) {
        real_dlsym = (DlsymFn)real_dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
        if (!real_dlsym) real_dlsym = (DlsymFn)real_dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    }
}

XFSR_EXPORT void* dlsym(void* handle, const char* symbol) {
    ensure_dlsym();
    if (!real_dlsym) return NULL;
    if (symbol && strcmp(symbol, "dlsym") == 0) return (void*)dlsym;
    return wrap_proc(symbol, real_dlsym(handle, symbol));
}

static void* hook_get_proc(GetProcFn real, const char* name) {
    if (!real) return NULL;
    return wrap_proc(name, real(name));
}

XFSR_EXPORT void* glXGetProcAddress(const char* name) {
    static GetProcFn real = NULL;
    if (!real) real = (GetProcFn)resolve_next("glXGetProcAddress", (void*)glXGetProcAddress);
    return hook_get_proc(real, name);
}

XFSR_EXPORT void* glXGetProcAddressARB(const char* name) {
    static GetProcFn real = NULL;
    if (!real) {
        real = (GetProcFn)resolve_next("glXGetProcAddressARB", (void*)glXGetProcAddressARB);
        if (!real) real = (GetProcFn)resolve_next("glXGetProcAddress", (void*)glXGetProcAddress);
    }
    return hook_get_proc(real, name);
}

XFSR_EXPORT void* eglGetProcAddress(const char* name) {
    static GetProcFn real = NULL;
    if (!real) real = (GetProcFn)resolve_next("eglGetProcAddress", (void*)eglGetProcAddress);
    return hook_get_proc(real, name);
}

XFSR_EXPORT void* glfwGetProcAddress(const char* name) {
    static GetProcFn real = NULL;
    if (!real) real = (GetProcFn)resolve_next("glfwGetProcAddress", (void*)glfwGetProcAddress);
    return hook_get_proc(real, name);
}
