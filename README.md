# libnoisemaker

A native C++ library, with a C API, that runs [Noisemaker](https://noisemaker.app) programs on the GPU through [Dawn](https://dawn.googlesource.com/dawn).

This library is under construction. Nothing here is ready to use yet.

## Building

Requirements: CMake 3.22 or later, a C++20 compiler, Python 3.12 or later, Node.js 26, Git and Ninja (or Visual Studio 2022 on Windows).

    python3 tools/fetch_dawn.py
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

## License

MIT. Dawn and its dependencies carry their own licenses (BSD-3-Clause, Apache-2.0, MIT).
