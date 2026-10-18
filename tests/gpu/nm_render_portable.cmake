# ctest driver: nm-render and Portable sidecars (<name>.portable.json beside
# a --dsl program). A valid sidecar registers its effect and the program
# renders. A malformed sidecar registers nothing, so the program that calls
# its effect fails to compile: exit 1, the compiler's unknown-effect error,
# no image.
#   cmake -DNM_RENDER=... -DSOURCE_DIR=... -DWORK_DIR=... -P nm_render_portable.cmake
# NM_ALLOW_FALLBACK_ADAPTER in the environment passes --allow-fallback.
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(fallback)
if(DEFINED ENV{NM_ALLOW_FALLBACK_ADAPTER})
  set(fallback --allow-fallback)
endif()

function(render name expect_code expect_text)
  set(out "${WORK_DIR}/${name}.png")
  execute_process(COMMAND "${NM_RENDER}" render --dsl "${WORK_DIR}/${name}.dsl" --out "${out}" ${fallback}
    RESULT_VARIABLE code ERROR_VARIABLE err OUTPUT_VARIABLE out_text)
  if(NOT code STREQUAL "${expect_code}")
    message(FATAL_ERROR "${name}: exit ${code}, expected ${expect_code}\n${err}")
  endif()
  if(expect_text AND NOT err MATCHES "${expect_text}")
    message(FATAL_ERROR "${name}: stderr does not match '${expect_text}'\n${err}")
  endif()
  if(expect_code STREQUAL "0" AND NOT EXISTS "${out}")
    message(FATAL_ERROR "${name}: exit 0 without ${out}")
  endif()
  if(NOT expect_code STREQUAL "0" AND EXISTS "${out}")
    message(FATAL_ERROR "${name}: exit ${code} but wrote ${out}")
  endif()
  message(STATUS "${name}: exit ${code}")
endfunction()

set(program "search user\nrings(freq: 11, shape: squareRings).write(o0)\nrender(o0)\n")
file(READ "${SOURCE_DIR}/parity/portable/portableRings.portable.json" sidecar)
file(READ "${SOURCE_DIR}/parity/portable/portableRings.rings.wgsl" wgsl)
file(READ "${SOURCE_DIR}/parity/portable/portableRings.rings.glsl" glsl)
set(unknown "Unknown effect: 'rings'")

# The parity fixture's sidecar and shaders, under another name.
file(WRITE "${WORK_DIR}/valid.dsl" "${program}")
file(WRITE "${WORK_DIR}/valid.portable.json" "${sidecar}")
file(WRITE "${WORK_DIR}/valid.rings.wgsl" "${wgsl}")
file(WRITE "${WORK_DIR}/valid.rings.glsl" "${glsl}")
render(valid 0 "")

# The sidecar is JSON, but not an object.
file(WRITE "${WORK_DIR}/array.dsl" "${program}")
file(WRITE "${WORK_DIR}/array.portable.json" "[1, 2]\n")
file(WRITE "${WORK_DIR}/array.rings.wgsl" "${wgsl}")
render(array 1 "${unknown}")

# The definition has no passes.
file(WRITE "${WORK_DIR}/nopasses.dsl" "${program}")
file(WRITE "${WORK_DIR}/nopasses.portable.json" "{ \"name\": \"Rings\", \"namespace\": \"user\", \"func\": \"rings\", \"globals\": {} }\n")
file(WRITE "${WORK_DIR}/nopasses.rings.wgsl" "${wgsl}")
render(nopasses 1 "${unknown}")

# A pass names a program with no shader source: no <name>.rings.wgsl or
# .glsl beside the sidecar, and none inline.
file(WRITE "${WORK_DIR}/noshader.dsl" "${program}")
file(WRITE "${WORK_DIR}/noshader.portable.json" "${sidecar}")
render(noshader 1 "${unknown}")
