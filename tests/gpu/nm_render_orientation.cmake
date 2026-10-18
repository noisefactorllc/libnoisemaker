# ctest driver: nm-render writes its PNG as the reference page presents the
# frame. It renders parity/coverage/synth_testPattern.dsl (a grid of the
# digits 0 to 15, 0 to 3 across the top) with the golden protocol and checks
# the result against testpattern_presented.png with png_layout.
#   cmake -DNM_RENDER=... -DPNG_LAYOUT=... -DSOURCE_DIR=... -DWORK_DIR=... -P nm_render_orientation.cmake
# NM_ALLOW_FALLBACK_ADAPTER in the environment passes --allow-fallback.
#
# testpattern_presented.png is the reference page's presented frame, not
# nm-render output: Noisemaker 1.0.295 (13eb5fb69c582bcbf5344c3dd3824da51a449b74),
# its demo page /demo/shaders/ in headless Chromium 153.0.8010.12 (Playwright
# 1.63.0) on the WebGPU backend (Apple M2, Metal), 256 x 256, 8 frames at
# time 0.25, read by parity/batch-golden.mjs as the canvas texture that the
# last frame's present() wrote (a copy of context.getCurrentTexture() in
# that call), rows top first.
# SHA-256 450421c453108b4f3a365013467833766f8bde69499a4e52cc1a301159027e01.
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(fallback)
if(DEFINED ENV{NM_ALLOW_FALLBACK_ADAPTER})
  set(fallback --allow-fallback)
endif()
set(golden "${SOURCE_DIR}/tests/gpu/testpattern_presented.png")
file(SHA256 "${golden}" sha)
if(NOT sha STREQUAL "450421c453108b4f3a365013467833766f8bde69499a4e52cc1a301159027e01")
  message(FATAL_ERROR "testpattern_presented.png has SHA-256 ${sha}")
endif()
execute_process(COMMAND "${NM_RENDER}" render --dsl "${SOURCE_DIR}/parity/coverage/synth_testPattern.dsl"
  --out "${WORK_DIR}/testpattern.png" ${fallback} RESULT_VARIABLE code ERROR_VARIABLE err)
if(code)
  message(FATAL_ERROR "nm-render exited ${code}\n${err}")
endif()
execute_process(COMMAND "${PNG_LAYOUT}" "${WORK_DIR}/testpattern.png" "${golden}" RESULT_VARIABLE code
  OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "${out}${err}")
if(code)
  message(FATAL_ERROR "nm-render's testPattern does not have the presented layout (png_layout exited ${code})")
endif()
