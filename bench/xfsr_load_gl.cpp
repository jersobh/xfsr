// Uncapped OpenGL load for xfsr-run (via Zink → Vulkan layer).
// Loads GL 3.3 entry points through GLFW — no glad/epoxy dependency.
//
// Usage:
//   ./build/xfsr_load_gl [--seconds N] [--complexity N] [--width W] [--height H]
//   xfsr-run --preset performance ./build/xfsr_load_gl --seconds 8

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLchar = char;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TRIANGLES 0x0004
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_RENDERER 0x1F01
#define GL_ARRAY_BUFFER 0x8892

namespace {

template <typename T>
T load(const char* name) {
    return reinterpret_cast<T>(glfwGetProcAddress(name));
}

using PFN_glClear = void (*)(GLbitfield);
using PFN_glClearColor = void (*)(GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glViewport = void (*)(GLint, GLint, GLsizei, GLsizei);
using PFN_glDrawArrays = void (*)(GLenum, GLint, GLsizei);
using PFN_glGetString = const unsigned char* (*)(GLenum);
using PFN_glCreateShader = GLuint (*)(GLenum);
using PFN_glShaderSource = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader = void (*)(GLuint);
using PFN_glGetShaderiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetShaderInfoLog = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glCreateProgram = GLuint (*)();
using PFN_glAttachShader = void (*)(GLuint, GLuint);
using PFN_glLinkProgram = void (*)(GLuint);
using PFN_glGetProgramiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetProgramInfoLog = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glDeleteShader = void (*)(GLuint);
using PFN_glDeleteProgram = void (*)(GLuint);
using PFN_glUseProgram = void (*)(GLuint);
using PFN_glGetUniformLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glUniform1f = void (*)(GLint, GLfloat);
using PFN_glUniform1i = void (*)(GLint, GLint);
using PFN_glGenVertexArrays = void (*)(GLsizei, GLuint*);
using PFN_glBindVertexArray = void (*)(GLuint);
using PFN_glDeleteVertexArrays = void (*)(GLsizei, const GLuint*);

PFN_glClear p_glClear = nullptr;
PFN_glClearColor p_glClearColor = nullptr;
PFN_glViewport p_glViewport = nullptr;
PFN_glDrawArrays p_glDrawArrays = nullptr;
PFN_glGetString p_glGetString = nullptr;
PFN_glCreateShader p_glCreateShader = nullptr;
PFN_glShaderSource p_glShaderSource = nullptr;
PFN_glCompileShader p_glCompileShader = nullptr;
PFN_glGetShaderiv p_glGetShaderiv = nullptr;
PFN_glGetShaderInfoLog p_glGetShaderInfoLog = nullptr;
PFN_glCreateProgram p_glCreateProgram = nullptr;
PFN_glAttachShader p_glAttachShader = nullptr;
PFN_glLinkProgram p_glLinkProgram = nullptr;
PFN_glGetProgramiv p_glGetProgramiv = nullptr;
PFN_glGetProgramInfoLog p_glGetProgramInfoLog = nullptr;
PFN_glDeleteShader p_glDeleteShader = nullptr;
PFN_glDeleteProgram p_glDeleteProgram = nullptr;
PFN_glUseProgram p_glUseProgram = nullptr;
PFN_glGetUniformLocation p_glGetUniformLocation = nullptr;
PFN_glUniform1f p_glUniform1f = nullptr;
PFN_glUniform1i p_glUniform1i = nullptr;
PFN_glGenVertexArrays p_glGenVertexArrays = nullptr;
PFN_glBindVertexArray p_glBindVertexArray = nullptr;
PFN_glDeleteVertexArrays p_glDeleteVertexArrays = nullptr;

void die(const char* msg) {
    std::fprintf(stderr, "xfsr_load_gl: %s\n", msg);
    std::exit(1);
}

void loadGl() {
#define L(name) p_##name = load<PFN_##name>(#name); if (!p_##name) die("missing " #name)
    L(glClear);
    L(glClearColor);
    L(glViewport);
    L(glDrawArrays);
    L(glGetString);
    L(glCreateShader);
    L(glShaderSource);
    L(glCompileShader);
    L(glGetShaderiv);
    L(glGetShaderInfoLog);
    L(glCreateProgram);
    L(glAttachShader);
    L(glLinkProgram);
    L(glGetProgramiv);
    L(glGetProgramInfoLog);
    L(glDeleteShader);
    L(glDeleteProgram);
    L(glUseProgram);
    L(glGetUniformLocation);
    L(glUniform1f);
    L(glUniform1i);
    L(glGenVertexArrays);
    L(glBindVertexArray);
    L(glDeleteVertexArrays);
#undef L
}

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, nullptr);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "shader error: %s\n", log);
        die("compileShader");
    }
    return s;
}

GLuint linkProgram(GLuint vs, GLuint fs) {
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, vs);
    p_glAttachShader(p, fs);
    p_glLinkProgram(p);
    GLint ok = 0;
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "link error: %s\n", log);
        die("linkProgram");
    }
    return p;
}

const char* kVert = R"(#version 330 core
out vec2 uv;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kFrag = R"(#version 330 core
in vec2 uv;
out vec4 outColor;
uniform float uTime;
uniform int uComplexity;
uniform float uAspect;

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
  p.x *= max(uAspect, 0.1);
  vec3 ro = vec3(0.0, 0.35, 3.2);
  vec3 rd = normalize(vec3(p, -1.6));
  mat3 R = rotY(uTime * 0.9) * rotX(uTime * 0.45);
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
    int nC = max(uComplexity, 1);
    float v = 0.0;
    vec2 s = q.xy;
    for (int i = 0; i < nC; ++i) {
      float fi = float(i);
      s = abs(s) / (dot(s, s) + 0.001) - vec2(0.7 + 0.01 * sin(uTime + fi));
      v += exp(-dot(s, s));
    }
    col += 0.04 * vec3(v / float(nC));
  }
  outColor = vec4(col, 1.0);
}
)";

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
            std::printf("Usage: xfsr_load_gl [--seconds N] [--complexity N] [--width W] [--height H]\n");
            return 0;
        }
    }
    complexity = std::clamp(complexity, 1, 512);

    if (!glfwInit()) die("glfwInit");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(winW, winH, "xfsr_load_gl (uncapped)", nullptr, nullptr);
    if (!window) die("glfwCreateWindow");
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);
    loadGl();

    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    std::printf("GL renderer: %s\n", reinterpret_cast<const char*>(p_glGetString(GL_RENDERER)));
    std::printf("framebuffer %dx%d complexity=%d vsync=off\n", fbw, fbh, complexity);

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVert);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFrag);
    GLuint prog = linkProgram(vs, fs);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint locTime = p_glGetUniformLocation(prog, "uTime");
    GLint locCplx = p_glGetUniformLocation(prog, "uComplexity");
    GLint locAspect = p_glGetUniformLocation(prog, "uAspect");

    GLuint vao = 0;
    p_glGenVertexArrays(1, &vao);
    p_glBindVertexArray(vao);

    auto t0 = std::chrono::steady_clock::now();
    auto tReport = t0;
    uint64_t frames = 0, framesWindow = 0;
    double bestFps = 0;
    float time = 0.0f;

    std::printf("XFSR_BENCH_BEGIN api=opengl seconds=%d complexity=%d extent=%dx%d\n", seconds,
                complexity, fbw, fbh);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - t0).count() >= seconds) break;

        glfwGetFramebufferSize(window, &fbw, &fbh);
        p_glViewport(0, 0, fbw, fbh);
        p_glClearColor(0.12f, 0.13f, 0.16f, 1.0f);
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glUseProgram(prog);
        p_glUniform1f(locTime, time);
        p_glUniform1i(locCplx, complexity);
        if (locAspect >= 0)
            p_glUniform1f(locAspect, fbh > 0 ? static_cast<float>(fbw) / static_cast<float>(fbh) : 1.0f);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
        glfwSwapBuffers(window);

        ++frames;
        ++framesWindow;
        time += 0.016f;

        double dt = std::chrono::duration<double>(now - tReport).count();
        if (dt >= 0.5) {
            double fps = framesWindow / dt;
            bestFps = std::max(bestFps, fps);
            std::printf("FPS %.1f  (frames=%llu extent=%dx%d)\n", fps,
                        static_cast<unsigned long long>(frames), fbw, fbh);
            framesWindow = 0;
            tReport = now;
        }
    }

    double totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double avgFps = totalSec > 0 ? frames / totalSec : 0;
    std::printf("XFSR_BENCH_END api=opengl frames=%llu seconds=%.3f avg_fps=%.2f best_fps=%.2f "
                "extent=%dx%d complexity=%d\n",
                static_cast<unsigned long long>(frames), totalSec, avgFps, bestFps, fbw, fbh,
                complexity);

    p_glDeleteVertexArrays(1, &vao);
    p_glDeleteProgram(prog);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
