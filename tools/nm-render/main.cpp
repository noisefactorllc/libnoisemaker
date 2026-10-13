// nm-render — render a Noisemaker program on the GPU with the reference
// pipeline (src/gpu/backend) over Dawn (src/gpu/dawn_device.h).
//
// `render` follows the golden protocol of parity/batch-golden.mjs for one
// program: compile the DSL (or take a graph the minter wrote), create the
// pipeline at SIZE x SIZE, reset it to a fresh state (every
// pipeline-written texture cleared, double-buffered surfaces back to their
// read and write textures, frame index and clock zeroed, global uniforms
// emptied), render FRAMES frames at the normalized TIME, and write the
// render surface's read texture as the backend's readPixels() converts it,
// in texture row order (row 0 first).
//
// Not yet: host inputs (media, text, meshes, MIDI, the asyncInit overlays),
// timed sampling and the batch manifest.

#include "core/graph/dsl_compiler.h"
#include "core/lang/effect_registry.h"
#include "core/value/json.h"
#include "gpu/backend/backend.h"
#include "gpu/backend/pipeline.h"
#include "gpu/dawn_device.h"
#include "gpu/device.h"
#include "png.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nm = libnoisemaker;
namespace be = libnoisemaker::backend;

namespace {

const char* kUsage =
    "usage: nm-render render (--dsl FILE | --graph FILE) --out FILE.png [--size N | --width W --height H]\n"
    "                        [--time T] [--frames N] [--allow-fallback]\n"
    "\n"
    "Renders one program with the golden protocol of parity/batch-golden.mjs (defaults: size 256,\n"
    "time 0.25, 8 frames) and writes the render surface in texture row order.\n"
    "Exit status: 0 on success, 1 when the program fails to compile or render, 2 for usage errors.\n";

struct Options {
    std::string dsl;
    std::string graph;
    std::string out;
    double width = 256;
    double height = 256;
    double time = 0.25;
    int frames = 8;
    bool allowFallback = false;
};

bool read_file(const std::string& path, std::string& text) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

std::optional<Options> parse(int argc, char** argv) {
    Options options;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (arg == "--allow-fallback") {
            options.allowFallback = true;
            continue;
        }
        if (!(v = value())) return std::nullopt;
        if (arg == "--dsl") options.dsl = v;
        else if (arg == "--graph") options.graph = v;
        else if (arg == "--out") options.out = v;
        else if (arg == "--size") options.width = options.height = std::atof(v);
        else if (arg == "--width") options.width = std::atof(v);
        else if (arg == "--height") options.height = std::atof(v);
        else if (arg == "--time") options.time = std::atof(v);
        else if (arg == "--frames") options.frames = std::atoi(v);
        else return std::nullopt;
    }
    if (options.dsl.empty() == options.graph.empty() || options.out.empty()) return std::nullopt;
    if (!(options.width >= 1 && options.height >= 1 && options.frames >= 0)) return std::nullopt;
    return options;
}

nm::Value load_graph(const Options& options) {
    std::string text;
    const std::string& path = options.dsl.empty() ? options.graph : options.dsl;
    if (!read_file(path, text)) throw std::runtime_error("cannot read " + path);
    if (!options.graph.empty()) return nm::json::parse(text);
    static nm::EffectRegistry registry;
    static const bool loaded = (registry.loadEmbedded(), true);
    (void)loaded;
    return nm::Value(nm::compileGraphJson(nm::utf8_to_js(text), registry));
}

// /^global_mesh\d+_(positions|normals|uvs)$/
bool is_mesh_data(const nm::JsString& id) {
    const nm::JsString prefix = u"global_mesh";
    if (id.rfind(prefix, 0) != 0) return false;
    size_t i = prefix.size();
    while (i < id.size() && id[i] >= u'0' && id[i] <= u'9') ++i;
    if (i == prefix.size() || i >= id.size() || id[i] != u'_') return false;
    const nm::JsString rest = id.substr(i + 1);
    return rest == u"positions" || rest == u"normals" || rest == u"uvs";
}

// The minter's fresh state (batch-golden.mjs step 5).
void reset_to_fresh_state(be::Pipeline& pipeline, be::WebGPUBackend& backend) {
    const std::vector<nm::JsString> ids = backend.textureIds();
    for (const nm::JsString& id : ids) {
        const be::TextureRecord* tex = backend.texture(id);
        if (!tex || tex->isExternal || tex->is3D || tex->cube || id == u"midiNoteGrid" || is_mesh_data(id)) continue;
        backend.clearTexture(id);
    }
    for (const auto& [name, surface] : pipeline.surfaces()) {
        const nm::JsString read = u"global_" + name + u"_read";
        const nm::JsString write = u"global_" + name + u"_write";
        if (backend.texture(read) && backend.texture(write)) pipeline.setSurfaceTextures(name, read, write);
    }
    pipeline.resetFrameState();
}

int render(const Options& options) {
    nm::Value graph;
    try {
        graph = load_graph(options);
    } catch (const nm::JsError& e) {
        std::cerr << "nm-render: " << nm::js_to_utf8(e.name()) << ": " << nm::js_to_utf8(e.message()) << '\n';
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "nm-render: " << e.what() << '\n';
        return 1;
    }

    nm::gpu::DeviceOptions deviceOptions;
    deviceOptions.allow_fallback = options.allowFallback;
    deviceOptions.reference_requests = true;
    nm::gpu::DeviceResult created = nm::gpu::create_device(deviceOptions);
    if (created.status != nm::gpu::Status::Ok) {
        std::cerr << "nm-render: " << created.message << '\n';
        return 1;
    }
    nm::gpu::Device& device = created.device;
    std::cerr << "nm-render: adapter " << device.adapter_name << '\n';

    int status = 0;
    {
        nm::gpu::DawnDevice dawn(device);
        be::WebGPUBackend backend(dawn);
        try {
            be::Pipeline pipeline(graph, backend, {});
            pipeline.init(options.width, options.height);
            reset_to_fresh_state(pipeline, backend);
            for (int i = 0; i < options.frames; ++i) {
                pipeline.render(options.time);
                dawn.processEvents();
            }
            const nm::Value* surfaceName = graph.is_object() ? graph.as_object().find(u"renderSurface") : nullptr;
            const nm::JsString name = surfaceName && surfaceName->is_string() && !surfaceName->as_string().empty()
                                          ? surfaceName->as_string()
                                          : nm::JsString(u"o0");
            std::optional<nm::JsString> readId;
            for (const auto& [surface, record] : pipeline.surfaces()) {
                if (surface == name) readId = record.read;
            }
            if (!readId) throw nm::JsError(u"Error", u"no render surface " + name);
            const be::Pixels pixels = backend.readPixels(*readId);
            const auto width = uint32_t(nm::jsv::to_number(pixels.width));
            const auto height = uint32_t(nm::jsv::to_number(pixels.height));
            if (!nm_render::write_file(options.out, nm_render::encode_png_rgba8(width, height, pixels.data))) {
                std::cerr << "nm-render: cannot write " << options.out << '\n';
                status = 1;
            }
        } catch (const nm::JsError& e) {
            std::cerr << "nm-render: " << nm::js_to_utf8(e.name()) << ": " << nm::js_to_utf8(e.message()) << '\n';
            status = 1;
        } catch (const std::exception& e) {
            std::cerr << "nm-render: " << e.what() << '\n';
            status = 1;
        }
        dawn.waitIdle();
    }
    std::lock_guard<std::mutex> lock(device.errors->mutex);
    for (const std::string& error : device.errors->errors) std::cerr << "nm-render: WebGPU error: " << error << '\n';
    if (!device.errors->errors.empty()) status = 1;
    if (device.errors->lost) {
        std::cerr << "nm-render: device lost: " << device.errors->lost_message << '\n';
        status = 1;
    }
    return status;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) != "render") {
        std::cerr << kUsage;
        return 2;
    }
    const std::optional<Options> options = parse(argc, argv);
    if (!options) {
        std::cerr << kUsage;
        return 2;
    }
    return render(*options);
}
