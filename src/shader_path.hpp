#pragma once

#include <cstdlib>
#include <string>

// SPIR-V search order: XFSR_SHADER_DIR → compile-time SHADER_DIR (build tree).
inline std::string xfsrShaderDir() {
    if (const char* e = std::getenv("XFSR_SHADER_DIR")) {
        if (e[0]) return e;
    }
#ifdef SHADER_DIR
    return SHADER_DIR;
#else
    return "shaders";
#endif
}

inline std::string xfsrShaderPath(const char* file) {
    return xfsrShaderDir() + "/" + file;
}
