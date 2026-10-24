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

static void remap_rect(GLint* x, GLint* y, GLsizei* w, GLsizei* h) {
    load_extents();
    if (!w || !h) return;
    const GLint ox = x ? *x : 0;
    const GLint oy = y ? *y : 0;
    const GLsizei ow = *w, oh = *h;

    /* Already at render size — never shrink twice (640→320 with scale=2). */
    if (g_rw > 0 && g_rh > 0 && *w >= g_rw - 1 && *w <= g_rw + 1 && *h >= g_rh - 1 &&
        *h <= g_rh + 1)
        return;

    /* Zink's color image follows the caps-shrunk swapchain (render size) while
     * GLFW/GL still report the window. Remap full-window viewports down so we
     * fill that image. Never blindly divide arbitrary sizes by XFSR_SCALE —
     * that turned glmark2's 800x600 window into 320x240 inside an 800x600 FB
     * (postage stamp) when DISPLAY_EXTENT was still 1080p. */
    const int near = 2;
    const int is_display =
        (g_dw > 0 && g_dh > 0 && abs((int)*w - g_dw) <= near && abs((int)*h - g_dh) <= near);
    const int is_glfw_fb =
        (g_fb_w > 0 && g_fb_h > 0 && abs((int)*w - g_fb_w) <= near && abs((int)*h - g_fb_h) <= near);
    if (g_rw > 0 && g_rh > 0 && ox == 0 && oy == 0 && (is_display || is_glfw_fb)) {
        if (x) *x = 0;
        if (y) *y = 0;
        *w = g_rw;
        *h = g_rh;
    } else if (g_rw > 0 && g_rh > 0 && g_scale >= 1.01f && ox == 0 && oy == 0 && *w > 64 &&
               *h > 64) {
        /* Viewport matches window≈scale*render (app ignored our env display hint). */
        const int expect_w = (int)(g_rw * g_scale + 0.5f);
        const int expect_h = (int)(g_rh * g_scale + 0.5f);
        if (abs((int)*w - expect_w) <= 16 && abs((int)*h - expect_h) <= 16) {
            *w = g_rw;
            *h = g_rh;
        }
    }

    if (!g_logged && (*w != ow || *h != oh) && getenv("XFSR_VERBOSE")) {
        g_logged = 1;
        fprintf(stderr, "[xfsr-gl] remap %dx%d → %dx%d (glfw_fb=%dx%d)\n", (int)ow, (int)oh,
                (int)*w, (int)*h, g_fb_w, g_fb_h);
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
    /* Report render size when the window is at display size so the app's
     * glViewport matches Zink's caps-shrunk color image (and MangoHud). */
    if (g_dw > 0 && g_dh > 0 && g_rw > 0 && g_rh > 0 && *width >= g_dw - 1 &&
        *width <= g_dw + 1 && *height >= g_dh - 1 && *height <= g_dh + 1) {
        *width = g_rw;
        *height = g_rh;
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
