cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" XFSR_VERSION_RAW)
string(STRIP "${XFSR_VERSION_RAW}" XFSR_VERSION)
project(xfsr VERSION ${XFSR_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POLICY_DEFAULT_CMP0169 OLD)

option(XFSR_BUILD_DEMO "Build xfsr FSR3 demo (needs FidelityFX SDK libs)" ON)
option(XFSR_BUILD_BENCH "Build xfsr_load_vk / xfsr_load_gl bench apps" ON)

message(STATUS "xfsr / xfsr-run version ${XFSR_VERSION}")

include(FetchContent)

# Headers only — avoid Vulkan-Headers' C++ module target (breaks Makefiles).
FetchContent_Declare(
  vulkan_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG        v1.3.296
  GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(vulkan_headers)
if(NOT vulkan_headers_POPULATED)
  FetchContent_Populate(vulkan_headers)
endif()

FetchContent_Declare(
  volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG        1.3.295
  GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(volk)
if(NOT volk_POPULATED)
  FetchContent_Populate(volk)
endif()

# FSR 1.0 headers only (vendored) — full GPUOpen repo pulls huge sample submodules.
set(FFX_FSR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx-fsr)
if(NOT EXISTS ${FFX_FSR_DIR}/ffx_fsr1.h)
  message(FATAL_ERROR "Missing ${FFX_FSR_DIR}/ffx_fsr1.h — vendor AMD FidelityFX FSR 1.0 headers")
endif()

# FSR 3.1.4 (FidelityFX SDK 1.1.4 Linux fork) — prebuilt static libs under sdk/bin.
set(FFX_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/FidelityFX-SDK/sdk)
set(FFX_SDK_LIB_DIR ${FFX_SDK_DIR}/bin/ffx_sdk)
set(XFSR_HAVE_FSR3_LIBS FALSE)
if(EXISTS ${FFX_SDK_LIB_DIR}/libffx_fsr3upscaler_x64.a AND EXISTS ${FFX_SDK_LIB_DIR}/libffx_backend_vk_x64.a)
  set(XFSR_HAVE_FSR3_LIBS TRUE)
elseif(XFSR_BUILD_DEMO)
  message(WARNING "FSR3 libs missing under ${FFX_SDK_LIB_DIR} — demo disabled (layer still builds)")
  set(XFSR_BUILD_DEMO OFF)
endif()

find_program(GLSLC glslc REQUIRED)

if(XFSR_BUILD_DEMO OR XFSR_BUILD_BENCH)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLFW3 REQUIRED glfw3)
endif()

set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_BIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BIN_DIR})

set(SPIRV_OUTPUTS)

function(compile_shader NAME STAGE)
  set(SRC ${SHADER_SRC_DIR}/${NAME}.glsl)
  set(OUT ${SHADER_BIN_DIR}/${NAME}.spv)
  add_custom_command(
    OUTPUT ${OUT}
    COMMAND ${GLSLC} -O -fshader-stage=${STAGE} -o ${OUT} ${SRC}
    DEPENDS ${SRC}
    COMMENT "Compiling ${NAME}.glsl"
    VERBATIM
  )
  set(SPIRV_OUTPUTS ${SPIRV_OUTPUTS} ${OUT} PARENT_SCOPE)
endfunction()
compile_shader(scene.vert vertex)
compile_shader(scene.frag fragment)
compile_shader(blit.vert vertex)
compile_shader(blit.frag fragment)

set(FFX_INCLUDE ${FFX_FSR_DIR})
# FP32 (portable) + FP16 (Vega/RDNA) FSR1 compute variants
foreach(PREC fp32 fp16)
  if(PREC STREQUAL "fp32")
    set(SLOW 1)
  else()
    set(SLOW 0)
  endif()
  add_custom_command(
    OUTPUT ${SHADER_BIN_DIR}/fsr_easu_${PREC}.comp.spv
    COMMAND ${GLSLC} -O -fshader-stage=compute
            -DSAMPLE_EASU=1 -DSAMPLE_SLOW_FALLBACK=${SLOW}
            -I${FFX_INCLUDE}
            -o ${SHADER_BIN_DIR}/fsr_easu_${PREC}.comp.spv
            ${SHADER_SRC_DIR}/fsr_pass.comp.glsl
    DEPENDS ${SHADER_SRC_DIR}/fsr_pass.comp.glsl ${FFX_INCLUDE}/ffx_fsr1.h ${FFX_INCLUDE}/ffx_a.h
    COMMENT "Compiling fsr_easu_${PREC}.comp.spv"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${SHADER_BIN_DIR}/fsr_rcas_${PREC}.comp.spv
    COMMAND ${GLSLC} -O -fshader-stage=compute
            -DSAMPLE_RCAS=1 -DSAMPLE_SLOW_FALLBACK=${SLOW}
            -I${FFX_INCLUDE}
            -o ${SHADER_BIN_DIR}/fsr_rcas_${PREC}.comp.spv
            ${SHADER_SRC_DIR}/fsr_pass.comp.glsl
    DEPENDS ${SHADER_SRC_DIR}/fsr_pass.comp.glsl ${FFX_INCLUDE}/ffx_fsr1.h ${FFX_INCLUDE}/ffx_a.h
    COMMENT "Compiling fsr_rcas_${PREC}.comp.spv"
    VERBATIM
  )
  list(APPEND SPIRV_OUTPUTS
    ${SHADER_BIN_DIR}/fsr_easu_${PREC}.comp.spv
    ${SHADER_BIN_DIR}/fsr_rcas_${PREC}.comp.spv
  )
endforeach()
# Keep legacy names as aliases to fp32 for older tooling
add_custom_command(
  OUTPUT ${SHADER_BIN_DIR}/fsr_easu.comp.spv ${SHADER_BIN_DIR}/fsr_rcas.comp.spv
  COMMAND ${CMAKE_COMMAND} -E copy ${SHADER_BIN_DIR}/fsr_easu_fp32.comp.spv ${SHADER_BIN_DIR}/fsr_easu.comp.spv
  COMMAND ${CMAKE_COMMAND} -E copy ${SHADER_BIN_DIR}/fsr_rcas_fp32.comp.spv ${SHADER_BIN_DIR}/fsr_rcas.comp.spv
  DEPENDS ${SHADER_BIN_DIR}/fsr_easu_fp32.comp.spv ${SHADER_BIN_DIR}/fsr_rcas_fp32.comp.spv
  COMMENT "Aliasing fsr_* to fp32"
  VERBATIM
)
list(APPEND SPIRV_OUTPUTS ${SHADER_BIN_DIR}/fsr_easu.comp.spv ${SHADER_BIN_DIR}/fsr_rcas.comp.spv)
add_custom_target(shaders ALL DEPENDS ${SPIRV_OUTPUTS})

set(XFSR_CORE_SOURCES
  src/dual_devices.cpp
  src/frame_transfer.cpp
  src/fsr_pass.cpp
  src/gpu_image.cpp
  src/present.cpp
)

# Shared dual-GPU FSR core for the demo binary
add_library(xfsr_core STATIC
  ${XFSR_CORE_SOURCES}
  ${volk_SOURCE_DIR}/volk.c
)
add_dependencies(xfsr_core shaders)
target_include_directories(xfsr_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${vulkan_headers_SOURCE_DIR}/include
  ${volk_SOURCE_DIR}
  ${FFX_FSR_DIR}
)
target_compile_definitions(xfsr_core PUBLIC
  VK_NO_PROTOTYPES
  SHADER_DIR="${SHADER_BIN_DIR}"
)
target_compile_options(xfsr_core PRIVATE -Wall -Wextra)
target_link_libraries(xfsr_core PUBLIC ${CMAKE_DL_LIBS})

if(XFSR_BUILD_DEMO AND XFSR_HAVE_FSR3_LIBS)
  add_executable(xfsr-demo
    src/main.cpp
    src/vulkan_context.cpp
    src/scene_renderer.cpp
    src/fsr3_pass.cpp
    src/bench.cpp
    src/hud_overlay.cpp
  )

  add_dependencies(xfsr-demo shaders)

  target_include_directories(xfsr-demo PRIVATE
    ${GLFW3_INCLUDE_DIRS}
    ${FFX_SDK_DIR}/include
  )

  target_compile_definitions(xfsr-demo PRIVATE
    FFX_FSR_DIR="${FFX_FSR_DIR}"
  )

  target_link_libraries(xfsr-demo PRIVATE
    xfsr_core
    ${GLFW3_LIBRARIES}
    ${FFX_SDK_LIB_DIR}/libffx_fsr3upscaler_x64.a
    ${FFX_SDK_LIB_DIR}/libffx_backend_vk_x64.a
  )
  target_compile_options(xfsr-demo PRIVATE ${GLFW3_CFLAGS_OTHER} -Wall -Wextra)
  set_source_files_properties(src/fsr3_pass.cpp PROPERTIES COMPILE_FLAGS "-fno-strict-aliasing")

  if(UNIX AND NOT APPLE)
    target_link_libraries(xfsr-demo PRIVATE m)
  endif()
endif()

# --- VK_LAYER_XFSR (implicit Vulkan layer) ---
# Separate OBJECT lib so rename macros do not leak into the demo's xfsr_core.
add_library(xfsr_core_layer OBJECT
  ${XFSR_CORE_SOURCES}
  ${volk_SOURCE_DIR}/volk.c
  layer/xfsr_layer.cpp
  layer/xfsr_controls.cpp
)
add_dependencies(xfsr_core_layer shaders)
target_include_directories(xfsr_core_layer PRIVATE
  ${vulkan_headers_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/layer
  ${volk_SOURCE_DIR}
  ${FFX_FSR_DIR}
)
target_compile_definitions(xfsr_core_layer PRIVATE
  VK_NO_PROTOTYPES
  SHADER_DIR="${SHADER_BIN_DIR}"
  vkGetInstanceProcAddr=volk_vkGetInstanceProcAddr
  vkGetDeviceProcAddr=volk_vkGetDeviceProcAddr
)
target_compile_options(xfsr_core_layer PRIVATE -Wall -Wextra -fvisibility=hidden)
set_target_properties(xfsr_core_layer PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(VkLayer_XFSR SHARED
  layer/xfsr_layer_entry.c
  $<TARGET_OBJECTS:xfsr_core_layer>
)
target_include_directories(VkLayer_XFSR PRIVATE
  ${vulkan_headers_SOURCE_DIR}/include
)
target_compile_definitions(VkLayer_XFSR PRIVATE VK_NO_PROTOTYPES)
target_compile_options(VkLayer_XFSR PRIVATE -fvisibility=hidden)
target_link_libraries(VkLayer_XFSR PRIVATE ${CMAKE_DL_LIBS} stdc++)
# Proton/steamrt ships glibc 2.31; a host-built layer needs GLIBC_2.34+ (dlopen).
# Prefer pressure-vessel's host-lib overrides (same mechanism as MangoHud under /usr)
# so dlopen succeeds inside the container; fall back to /usr/lib on the bare host.
# Use classic DT_RPATH (--disable-new-dtags): Steam sets LD_LIBRARY_PATH, which
# would otherwise beat DT_RUNPATH and keep resolving steamrt's older libc.
target_link_options(VkLayer_XFSR PRIVATE
  "LINKER:--disable-new-dtags"
  "LINKER:-rpath,/usr/lib/pressure-vessel/overrides/lib/x86_64-linux-gnu"
  "LINKER:-rpath,/usr/lib/pressure-vessel/overrides/lib/i386-linux-gnu"
  "LINKER:-rpath,/usr/lib"
)
set_target_properties(VkLayer_XFSR PROPERTIES
  PREFIX ""
  OUTPUT_NAME "libVkLayer_XFSR"
  POSITION_INDEPENDENT_CODE ON
  BUILD_RPATH_USE_ORIGIN TRUE
)

set(LAYER_LIB_PATH ${CMAKE_CURRENT_BINARY_DIR}/libVkLayer_XFSR.so)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/layer/VkLayer_XFSR.json.in
  ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_XFSR.json
  @ONLY
)

if(XFSR_BUILD_BENCH)
  # Uncapped load apps for xfsr-run benchmarking (not vsync-capped like vkcube).
  add_executable(xfsr_load_vk
    bench/xfsr_load_vk.cpp
    ${volk_SOURCE_DIR}/volk.c
  )
  target_include_directories(xfsr_load_vk PRIVATE
    ${GLFW3_INCLUDE_DIRS}
    ${vulkan_headers_SOURCE_DIR}/include
    ${volk_SOURCE_DIR}
  )
  target_compile_definitions(xfsr_load_vk PRIVATE VK_NO_PROTOTYPES)
  target_compile_options(xfsr_load_vk PRIVATE ${GLFW3_CFLAGS_OTHER} -Wall -Wextra)
  target_link_libraries(xfsr_load_vk PRIVATE ${GLFW3_LIBRARIES} ${CMAKE_DL_LIBS})

  add_executable(xfsr_load_gl bench/xfsr_load_gl.cpp)
  target_include_directories(xfsr_load_gl PRIVATE ${GLFW3_INCLUDE_DIRS})
  target_compile_options(xfsr_load_gl PRIVATE ${GLFW3_CFLAGS_OTHER} -Wall -Wextra)
  target_link_libraries(xfsr_load_gl PRIVATE ${GLFW3_LIBRARIES} ${CMAKE_DL_LIBS})
endif()

# OpenGL viewport remap for Zink + FSR (LD_PRELOAD from xfsr-run).
add_library(xfsr_gl_shim SHARED gl/xfsr_gl_shim.c)
set_target_properties(xfsr_gl_shim PROPERTIES
  OUTPUT_NAME "xfsr_gl_shim"
  PREFIX "lib"
)
target_link_libraries(xfsr_gl_shim PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(xfsr_gl_shim PRIVATE -fvisibility=hidden -Wall -Wextra)

# User install: ~/.local/bin/xfsr-run + layer under ~/.local/{lib,share/vulkan/...}
add_custom_target(install-xfsr-layer
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/install-xfsr-run.sh
  DEPENDS VkLayer_XFSR xfsr_gl_shim
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Installing xfsr-run + implicit layer to ~/.local"
)

# Release tarball: dist/xfsr-${VERSION}-linux-x86_64.tar.gz
add_custom_target(package-xfsr
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package-xfsr.sh
  DEPENDS VkLayer_XFSR xfsr_gl_shim
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Packaging xfsr-run release tarball into dist/"
)
