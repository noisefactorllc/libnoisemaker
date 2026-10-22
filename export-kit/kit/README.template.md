# {{NM_PROGRAM_NAME}}

This C++ project renders your Noisedeck program on the GPU. `engine/` is the source of [libnoisemaker](https://github.com/noisefactorllc/libnoisemaker): the Noisemaker DSL compiler, the reference WebGPU pipeline ported to C++, and `nm-render`, a command-line renderer built on [Dawn](https://dawn.googlesource.com/dawn), the WebGPU implementation in Chromium. `nm-render` compiles `program.dsl` and writes a PNG image.

## Requirements

- **CMake 3.22 or newer.**
- **A C++20 compiler.** The engine is tested with Apple Clang 16 and 17 (Xcode or its Command Line Tools, `xcode-select --install`) on macOS and with GCC 13 on Ubuntu 24.04.
- **Python 3.12 or newer**, as `python3`. CMake looks for it too.
- **Node.js 26 or newer**, as `node`. The build runs the effect catalog tools with it.
- **Ninja.** The engine is tested with Ninja 1.11 and 1.13.
- **Git.** The engine is tested with Git 2.39 and 2.55.
- **A network connection for the first build.**
- **A GPU that Dawn can use.** On macOS Dawn renders with Metal. On Linux it renders with Vulkan, so it needs a Vulkan driver (on Debian and Ubuntu, `libvulkan1` and your GPU's driver package).

The engine is tested on macOS on Apple silicon. There its parity tests render every test program with the reference engine's exact pixels, except five programs that draw text, whose glyph edges differ slightly. On Linux the engine builds and renders with Mesa's software Vulkan driver, but those renders have not been compared with the reference.

## Build

Unzip this folder and open a terminal in it. The commands here are for macOS and Linux. Build the renderer once:

```sh
python3 engine/tools/fetch_dawn.py
cmake -S engine -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNM_BUILD_TESTS=OFF
cmake --build build --target nm-render
```

**The first build downloads source code and data.**

- `fetch_dawn.py` fetches Dawn at the commit `engine/third_party/dawn.lock.json` pins, and the seven sources Dawn builds with (Abseil, SPIRV-Tools, SPIRV-Headers, Vulkan-Headers, Vulkan-Utility-Libraries, Jinja2 and MarkupSafe) at the commits that Dawn commit records, from `dawn.googlesource.com` and `chromium.googlesource.com`. It checks each commit id. The download is about 47 MB, and the unpacked sources take about 640 MB in `engine/.cache/dawn`.
- `cmake --build` downloads the effect catalog of the Noisemaker release that `engine/parity/reference.json` locks from <https://shaders.noisedeck.app> (211 files, about 4 MB) and checks each file's SHA-256 against the lock. It also clones Noisemaker at the locked commit from <https://github.com/noisefactorllc/noisemaker> into `engine/.cache/reference` (about 33 MB), for the shader sources the published catalog omits. The catalog goes to `engine/.cache/catalog`.

CMake writes the build to `build/` (about 130 MB), and the renderer to `build/nm-render`. The build compiles Dawn as well as the engine. On an Apple M2, with other builds running, the first build took about 7 minutes: 43 seconds to fetch Dawn, 20 seconds to configure and 6 minutes to compile. Once it is done, everything is on your disk: rendering never uses the network.

## Render it

```sh
NM_REFERENCE_ROOT=engine/.cache/reference build/nm-render render --dsl program.dsl --out out.png --size 512
```

The command writes a 512×512 `out.png` beside your program, the way Noisedeck shows it. Paths are relative to your working directory. `render` draws 8 frames at one moment of the program's 10 second loop (`--time 0.25` by default, a fraction of the loop).

On a machine without a hardware GPU adapter, such as a virtual machine or a server, add `--allow-fallback`. `nm-render` then uses a software adapter when no hardware one is available, for example Mesa's lavapipe on Linux (`mesa-vulkan-drivers` on Debian and Ubuntu):

```sh
NM_REFERENCE_ROOT=engine/.cache/reference build/nm-render render --dsl program.dsl --out out.png --size 512 --allow-fallback
```

Useful options:

- `--width W --height H` in place of `--size N` for a rectangular image.
- `--time T` renders another moment of the loop, from 0 to 1.
- `--frames N` draws N frames instead of 8. Programs with feedback, particles or simulations build up over frames.

`build/nm-render` without arguments prints every option. The renderer exits with status 0 when it wrote the image, 1 when the program does not compile or render, and 2 for a command-line error.

## Inputs

Noisedeck exports the program, not the media or devices it ran with. `nm-render` reads three kinds of input from the Noisemaker checkout that `NM_REFERENCE_ROOT` names. The build leaves one at the locked commit in `engine/.cache/reference`.

- **Media.** `media()` steps show the Noisemaker demo's test card. `--host-texture imageTex_step_N=IMAGE.png` gives the `media()` step N another image (a PNG). Steps are numbered from 0 in program order, and every call counts, `write()` included: in `noise().write(o1)` followed by `media().write(o0)`, the `media()` step is step 2.
- **Text.** `text()` steps draw with the Noisemaker demo's Nunito font. Text in another font family is not drawn.
- **Meshes.** A `meshLoader()` step shows its built-in mesh. `--obj MESH.obj` loads another mesh. `nm-render` still reads the built-in meshes, so it needs `NM_REFERENCE_ROOT` with `--obj` too.
- **MIDI and audio.** There is no live MIDI or audio input. Programs render as the Noisemaker demo renders them when the browser denies MIDI and microphone access.

A program with none of these steps renders without `NM_REFERENCE_ROOT`. Without it, a program with a text or mesh step, or a media step that has no `--host-texture`, fails with status 1 and says what is missing.

## What's inside

| Path | What it is |
| --- | --- |
| `program.dsl` | Your program's source, exactly as it was in Noisedeck. |
| `engine/CMakeLists.txt`, `engine/cmake/` | The CMake project and its Dawn build options. |
| `engine/include/noisemaker/` | The public C and C++ headers. |
| `engine/src/` | The compiler, the render graph, the host inputs (text, meshes, overlays, MIDI and audio state) and the GPU pipeline. |
| `engine/tools/nm-render/` | The command-line renderer. |
| `engine/tools/nm-dump/` | Inspection tools the CMake project defines; the build above does not compile them. |
| `engine/tools/fetch_dawn.py`, `engine/tools/build_dawn.py`, `engine/tools/catalog/` | The scripts that fetch Dawn and the effect catalog and embed the catalog in the library. |
| `engine/third_party/` | `dawn.lock.json`, the Dawn commit and the dependency commits the build uses, and in `licenses/` the license texts of the Dawn sources the build compiles. |
| `engine/parity/reference.json` | The Noisemaker release and commit the engine follows, and the SHA-256 of every catalog file. |
| `noisedeck-export.json` | What was exported, when, and from which port commit. |
| `LICENSES/` | Licenses for everything shipped here and built from it. |

`engine/` is self-contained: copied elsewhere, it builds the same way. You can delete `build/` at any time; the next build takes longer. `engine/.cache/` holds the downloads; deleting it makes the next build download them again.

## Using the library in your own code

The engine's library compiles Noisemaker programs: the C API in `engine/include/noisemaker/core.h` (`nm_compile`, the render graph as JSON, diagnostics, `nm_unparse`) and the C++ API in `core.hpp` and `value.hpp`. It does not render; rendering is `nm-render`'s job, and `engine/tools/nm-render/main.cpp` shows how it drives the GPU pipeline. After the build above, install the library and point your CMake project at it:

```sh
cmake --install build --prefix "$PWD/prefix"
```

```cmake
cmake_minimum_required(VERSION 3.22)
project(app C CXX)
find_package(noisemaker CONFIG REQUIRED COMPONENTS core)
add_executable(app main.c)
target_link_libraries(app PRIVATE noisemaker::core)
```

Configure your project with `-DCMAKE_PREFIX_PATH=/path/to/prefix`. The installed library is static and written in C++, so the project enables C++ even when its own code is C: CMake then links it with the C++ standard library. This `main.c` compiles a program and prints its render graph:

```c
#include <noisemaker/core.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char source[] = "search synth\nnoise().write(o0)\nrender(o0)\n";
    nm_compiled* compiled = NULL;
    nm_diagnostics* diagnostics = NULL;
    if (nm_compile(source, strlen(source), &compiled, &diagnostics) != NM_OK) return 1;
    printf("%s\n", nm_compiled_graph_json(compiled));
    nm_compiled_destroy(compiled);
    nm_diagnostics_destroy(diagnostics);
    return 0;
}
```

The installed headers need C++17 or C. The C++ API passes standard-library types across the library boundary, so a C++ program must use the compiler and standard library that built it: `prefix/share/noisemaker/TOOLCHAIN` names them. <https://github.com/noisefactorllc/libnoisemaker> documents the library.

## Effects used by this program

{{NM_EFFECT_LIST}}

## Editing it

Replace `program.dsl` with another Noisemaker program and run the same render command. The build is already done, so only the render repeats.

Noisedeck exported this program against Noisemaker `{{NM_ENGINE_VERSION}}`. The engine here is a separate implementation, tested for pixel parity against the Noisemaker release that `engine/parity/reference.json` locks. An effect added to Noisemaker after that release is not in its catalog.

## License

libnoisemaker and the Noisemaker engine are MIT licensed: `LICENSES/libnoisemaker-LICENSE.txt` and `LICENSES/noisemaker-MIT.txt`. The effect catalog contains code from other works, listed with their notices in `LICENSES/catalog-NOTICES.txt`. The engine contains ports of acorn (MIT, `LICENSES/acorn-LICENSE.txt`) and of parts of Skia (BSD 3-Clause, `LICENSES/Skia-LICENSE.txt`). The build compiles Dawn and its Tint shader compiler into the renderer (BSD 3-Clause, `LICENSES/Dawn-LICENSE.txt`), with Abseil (Apache 2.0, `LICENSES/abseil-cpp-LICENSE.txt`). On Linux it also compiles in SPIRV-Tools (Apache 2.0, `LICENSES/SPIRV-Tools-LICENSE.txt`), SPIRV-Headers (`LICENSES/SPIRV-Headers-LICENSE.txt`), Vulkan-Headers (Apache 2.0 or MIT, `LICENSES/Vulkan-Headers-*`) and Vulkan-Utility-Libraries (Apache 2.0, `LICENSES/Vulkan-Utility-Libraries-*`). Jinja2 and MarkupSafe only generate Dawn's code during the build. The Nunito font that `text()` draws with is licensed under the SIL Open Font License 1.1: `engine/.cache/reference/demo/font/Nunito/OFL.txt`. The test card is the Philips PM5544 test card, by Ebnz, modified by Tucvbif (Wikimedia Commons, File:Philips_PM5544.svg), CC BY 2.5, rasterized to 768×576 for the Noisemaker demo. Your program and the imagery it renders are yours.
