# ctest driver: a graph the minter wrote renders as its DSL does. nm-render
# renders synth_media.graph.json with --graph and parity/coverage/synth_media.dsl
# with --dsl, each with the same media image as host texture, and the two PNGs
# must be identical. The graph carries the media step's imageSize, which the
# page set to the image's size as it loaded it.
#   cmake -DNM_RENDER=... -DSOURCE_DIR=... -DWORK_DIR=... -P nm_render_graph.cmake
# NM_ALLOW_FALLBACK_ADAPTER in the environment passes --allow-fallback.
#
# synth_media.graph.json and synth_media.imageTex_step_0.png come from the
# reference page, not from nm-render: parity/batch-golden.mjs minted
# parity/coverage/synth_media.dsl with Noisemaker 1.0.295
# (13eb5fb69c582bcbf5344c3dd3824da51a449b74) in headless Chromium 153.0.8010.12
# (Playwright 1.63.0) on the WebGPU backend (Apple M2, Metal), 256 x 256. The
# graph is the page's after its media image loaded; the PNG is the page's
# imageTex_step_0 texture (the demo's testcard.png upload), rows as stored.
# SHA-256 as committed: synth_media.graph.json
# 291e18f3974989872410e5f052837aa1ace6814aecb7bdfaf92ebc05b3c6115f (LF line
# endings), synth_media.imageTex_step_0.png
# 8ae4fad963b0d190bfa312ea64ac29fd98fc8cfe2dcf07ed4401480a4aa5bb0b.
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(fallback)
if(DEFINED ENV{NM_ALLOW_FALLBACK_ADAPTER})
  set(fallback --allow-fallback)
endif()
set(image "${SOURCE_DIR}/tests/gpu/synth_media.imageTex_step_0.png")
file(SHA256 "${image}" sha)
if(NOT sha STREQUAL "8ae4fad963b0d190bfa312ea64ac29fd98fc8cfe2dcf07ed4401480a4aa5bb0b")
  message(FATAL_ERROR "synth_media.imageTex_step_0.png has SHA-256 ${sha}")
endif()
foreach(mode graph dsl)
  if(mode STREQUAL "graph")
    set(input --graph "${SOURCE_DIR}/tests/gpu/synth_media.graph.json")
  else()
    set(input --dsl "${SOURCE_DIR}/parity/coverage/synth_media.dsl")
  endif()
  execute_process(COMMAND "${NM_RENDER}" render ${input} --host-texture "imageTex_step_0=${image}"
    --out "${WORK_DIR}/${mode}.png" ${fallback} RESULT_VARIABLE code ERROR_VARIABLE err)
  if(code)
    message(FATAL_ERROR "nm-render --${mode} exited ${code}\n${err}")
  endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${WORK_DIR}/graph.png" "${WORK_DIR}/dsl.png" RESULT_VARIABLE differ)
if(differ)
  message(FATAL_ERROR "the graph of synth_media does not render as its DSL")
endif()
message(STATUS "synth_media: --graph and --dsl render identical PNGs")
