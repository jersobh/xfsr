file(REMOVE_RECURSE
  "CMakeFiles/shaders"
  "shaders/blit.frag.spv"
  "shaders/blit.vert.spv"
  "shaders/edge_pass.comp.spv"
  "shaders/fsr_easu.comp.spv"
  "shaders/fsr_easu_fp16.comp.spv"
  "shaders/fsr_easu_fp32.comp.spv"
  "shaders/fsr_rcas.comp.spv"
  "shaders/fsr_rcas_fp16.comp.spv"
  "shaders/fsr_rcas_fp32.comp.spv"
  "shaders/nis_pass.comp.spv"
  "shaders/scene.frag.spv"
  "shaders/scene.vert.spv"
)

# Per-language clean rules from dependency scanning.
foreach(lang )
  include(CMakeFiles/shaders.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
