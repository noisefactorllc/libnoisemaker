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
// `--host-texture ID=PNG` uploads a texture's contents (the minter saves
// each host input it binds, media, text and the asyncInit overlays, as
// <name>.<id>.png) as the page's canvas upload does, rows as they are.
// `batch MANIFEST` renders a JSON array of fixtures in one process and
// device, a fresh pipeline each: {"graph" | "dsl", "out", "size" or
// "width" and "height", "time", "frames", "hostTextures": {id: png}}, the
// names of rust-gpu's nm-render manifest; a fixture with any other member
// fails, as nm-render does not support it yet.
//
// Not yet: meshes, MIDI sidecars, Portable effects, the demo host's
// parameter application, timed sampling and texture dumps.

#include "core/graph/dsl_compiler.h"
#include "core/lang/effect_registry.h"
#include "core/value/json.h"
#include "gpu/backend/backend.h"
#include "gpu/backend/pipeline.h"
#include "gpu/dawn_device.h"
#include "gpu/device.h"
#include "png.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nm = libnoisemaker;
namespace be = libnoisemaker::backend;

namespace {

const char* kUsage =
    "usage: nm-render render (--dsl FILE | --graph FILE) --out FILE.png [--size N | --width W --height H]\n"
    "                        [--time T] [--frames N] [--host-texture ID=PNG ...] [--allow-fallback]\n"
    "       nm-render batch MANIFEST [--allow-fallback]\n"
    "\n"
    "Renders programs with the golden protocol of parity/batch-golden.mjs (defaults: size 256,\n"
    "time 0.25, 8 frames) and writes the render surface in texture row order.\n"
    "Exit status: 0 on success, 1 when a program fails to compile or render, 2 for usage errors.\n";

struct Fixture {
    std::string dsl;
    std::string graph;
    std::string out;
    double width = 256;
    double height = 256;
    double time = 0.25;
    int frames = 8;
    std::vector<std::pair<std::string, std::string>> hostTextures;
};

struct Usage : std::runtime_error {
    using std::runtime_error::runtime_error;
};

bool read_file(const std::string& path, std::string& text) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

// A whole argument as a finite number, or nothing.
std::optional<double> number(const char* text) {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value)) return std::nullopt;
    return value;
}

// Sizes are whole pixels, as the minter's --size is; frames a whole count.
void check(const Fixture& fixture) {
    const auto pixels = [](double v) { return v >= 1 && v <= 16384 && v == std::floor(v); };
    if (fixture.dsl.empty() == fixture.graph.empty()) throw Usage("give one of --dsl and --graph");
    if (fixture.out.empty()) throw Usage("--out is required");
    if (!pixels(fixture.width) || !pixels(fixture.height)) throw Usage("sizes are whole pixels from 1 to 16384");
    if (!std::isfinite(fixture.time)) throw Usage("the time is not a finite number");
    if (fixture.frames < 0 || fixture.frames > 1000000) throw Usage("frames are a whole count up to 1000000");
}

Fixture parse_render(int argc, char** argv, bool& allowFallback) {
    Fixture fixture;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--allow-fallback") {
            allowFallback = true;
            continue;
        }
        if (i + 1 >= argc) throw Usage(arg + " needs a value");
        const char* v = argv[++i];
        if (arg == "--dsl") {
            fixture.dsl = v;
        } else if (arg == "--graph") {
            fixture.graph = v;
        } else if (arg == "--out") {
            fixture.out = v;
        } else if (arg == "--host-texture") {
            const std::string pair = v;
            const size_t eq = pair.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 == pair.size()) throw Usage("--host-texture takes ID=PNG");
            fixture.hostTextures.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
        } else {
            const std::optional<double> n = number(v);
            if (!n) throw Usage(arg + " takes a number");
            if (arg == "--size") fixture.width = fixture.height = *n;
            else if (arg == "--width") fixture.width = *n;
            else if (arg == "--height") fixture.height = *n;
            else if (arg == "--time") fixture.time = *n;
            else if (arg == "--frames" && *n == std::floor(*n) && *n >= 0 && *n <= 1e6) fixture.frames = int(*n);
            else throw Usage("unknown option or value: " + arg);
        }
    }
    check(fixture);
    return fixture;
}

std::string string_member(const nm::Object& entry, const char16_t* key) {
    const nm::Value* value = entry.find(key);
    if (!value) return {};
    if (!value->is_string()) throw Usage(nm::js_to_utf8(key) + " must be a string");
    return nm::js_to_utf8(value->as_string());
}

std::optional<double> number_member(const nm::Object& entry, const char16_t* key) {
    const nm::Value* value = entry.find(key);
    if (!value) return std::nullopt;
    if (!value->is_number() || !std::isfinite(value->as_number())) throw Usage(nm::js_to_utf8(key) + " must be a finite number");
    return value->as_number();
}

// One manifest entry, or the reason it cannot run.
Fixture parse_entry(const nm::Value& value) {
    if (!value.is_object()) throw Usage("an entry is not an object");
    const nm::Object& entry = value.as_object();
    for (const auto& key : entry.keys()) {
        static const char16_t* supported[] = {u"graph", u"dsl", u"out", u"size", u"width", u"height", u"time", u"frames",
                                              u"hostTextures"};
        bool known = false;
        for (const char16_t* name : supported) known = known || key == name;
        if (!known) throw Usage("unsupported member " + nm::js_to_utf8(key));
    }
    Fixture fixture;
    fixture.graph = string_member(entry, u"graph");
    fixture.dsl = string_member(entry, u"dsl");
    fixture.out = string_member(entry, u"out");
    if (const auto size = number_member(entry, u"size")) fixture.width = fixture.height = *size;
    if (const auto width = number_member(entry, u"width")) fixture.width = *width;
    if (const auto height = number_member(entry, u"height")) fixture.height = *height;
    if (const auto time = number_member(entry, u"time")) fixture.time = *time;
    if (const auto frames = number_member(entry, u"frames")) {
        if (*frames != std::floor(*frames) || *frames < 0 || *frames > 1e6) throw Usage("frames must be a whole count");
        fixture.frames = int(*frames);
    }
    if (const nm::Value* host = entry.find(u"hostTextures")) {
        if (!host->is_object()) throw Usage("hostTextures must be an object");
        for (const auto& id : host->as_object().keys()) {
            const nm::Value* png = host->as_object().find(id);
            if (!png || !png->is_string()) throw Usage("a host texture path must be a string");
            fixture.hostTextures.emplace_back(nm::js_to_utf8(id), nm::js_to_utf8(png->as_string()));
        }
    }
    check(fixture);
    return fixture;
}

nm::Value load_graph(const Fixture& fixture) {
    std::string text;
    const std::string& path = fixture.dsl.empty() ? fixture.graph : fixture.dsl;
    if (!read_file(path, text)) throw std::runtime_error("cannot read " + path);
    if (!fixture.graph.empty()) return nm::json::parse(text);
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

// The minter's fresh state (batch-golden.mjs step 5); keep names the host
// textures it bound, which an upload into an existing texture of the same
// size leaves without isExternal.
void reset_to_fresh_state(be::Pipeline& pipeline, be::WebGPUBackend& backend, const std::vector<nm::JsString>& keep) {
    const std::vector<nm::JsString> ids = backend.textureIds();
    for (const nm::JsString& id : ids) {
        const be::TextureRecord* tex = backend.texture(id);
        if (!tex || std::find(keep.begin(), keep.end(), id) != keep.end()) continue;
        if (tex->isExternal || tex->is3D || tex->cube || id == u"midiNoteGrid" || is_mesh_data(id)) continue;
        backend.clearTexture(id);
    }
    for (const auto& [name, surface] : pipeline.surfaces()) {
        const nm::JsString read = u"global_" + name + u"_read";
        const nm::JsString write = u"global_" + name + u"_write";
        if (backend.texture(read) && backend.texture(write)) pipeline.setSurfaceTextures(name, read, write);
    }
    pipeline.resetFrameState();
}

// Renders one fixture on the device; 0 on success, 1 on failure (reported).
int render_fixture(nm::gpu::Device& device, const Fixture& fixture) {
    const std::string name = fixture.out;
    const auto fail = [&](const std::string& message) {
        std::cerr << "nm-render: " << name << ": " << message << '\n';
        return 1;
    };
    size_t errorsBefore = 0;
    {
        std::lock_guard<std::mutex> lock(device.errors->mutex);
        errorsBefore = device.errors->errors.size();
    }
    int status = 0;
    {
        nm::gpu::DawnDevice dawn(device);
        be::WebGPUBackend backend(dawn);
        try {
            const nm::Value graph = load_graph(fixture);
            be::Pipeline pipeline(graph, backend, {});
            pipeline.init(fixture.width, fixture.height);
            // The minter's captures: texture contents, uploaded with flipY false.
            std::vector<nm::JsString> keep;
            for (const auto& [id, path] : fixture.hostTextures) {
                const nm_render::Image image = nm_render::decode_png(nm_render::read_file(path));
                keep.push_back(nm::utf8_to_js(id));
                backend.updateTextureFromPixels(keep.back(), image.width, image.height, image.rgba, false);
            }
            reset_to_fresh_state(pipeline, backend, keep);
            for (int i = 0; i < fixture.frames; ++i) {
                pipeline.render(fixture.time);
                dawn.processEvents();
            }
            const nm::Value* surfaceName = graph.is_object() ? graph.as_object().find(u"renderSurface") : nullptr;
            const nm::JsString surface = surfaceName && surfaceName->is_string() && !surfaceName->as_string().empty()
                                             ? surfaceName->as_string()
                                             : nm::JsString(u"o0");
            std::optional<nm::JsString> readId;
            for (const auto& [id, record] : pipeline.surfaces()) {
                if (id == surface) readId = record.read;
            }
            if (!readId) throw nm::JsError(u"Error", u"no render surface " + surface);
            const be::Pixels pixels = backend.readPixels(*readId);
            const auto width = uint32_t(nm::jsv::to_number(pixels.width));
            const auto height = uint32_t(nm::jsv::to_number(pixels.height));
            if (!nm_render::write_file(fixture.out, nm_render::encode_png_rgba8(width, height, pixels.data))) {
                status = fail("cannot write the PNG");
            }
        } catch (const nm::JsError& e) {
            status = fail(nm::js_to_utf8(e.name()) + ": " + nm::js_to_utf8(e.message()));
        } catch (const std::exception& e) {
            status = fail(e.what());
        }
        dawn.waitIdle();
    }
    std::lock_guard<std::mutex> lock(device.errors->mutex);
    for (size_t i = errorsBefore; i < device.errors->errors.size(); ++i) {
        status = fail("WebGPU error: " + device.errors->errors[i]);
    }
    if (device.errors->lost) status = fail("device lost: " + device.errors->lost_message);
    return status;
}

std::optional<nm::gpu::DeviceResult> open_device(bool allowFallback) {
    nm::gpu::DeviceOptions options;
    options.allow_fallback = allowFallback;
    options.reference_requests = true;
    nm::gpu::DeviceResult created = nm::gpu::create_device(options);
    if (created.status != nm::gpu::Status::Ok) {
        std::cerr << "nm-render: " << created.message << '\n';
        return std::nullopt;
    }
    std::cerr << "nm-render: adapter " << created.device.adapter_name << '\n';
    return created;
}

int run_batch(const std::string& manifestPath, bool allowFallback) {
    std::string text;
    if (!read_file(manifestPath, text)) throw Usage("cannot read " + manifestPath);
    nm::Value manifest;
    try {
        manifest = nm::json::parse(text);
    } catch (const std::exception& e) {
        throw Usage(manifestPath + ": " + e.what());
    }
    if (!manifest.is_array()) throw Usage(manifestPath + ": the manifest is not an array");
    std::optional<nm::gpu::DeviceResult> created = open_device(allowFallback);
    if (!created) return 1;
    size_t failed = 0;
    const nm::Array& entries = manifest.as_array();
    for (size_t i = 0; i < entries.size(); ++i) {
        try {
            if (render_fixture(created->device, parse_entry(entries[i])) != 0) ++failed;
        } catch (const Usage& e) {
            std::cerr << "nm-render: entry " << i << ": " << e.what() << '\n';
            ++failed;
        }
    }
    std::cerr << "nm-render: batch: " << entries.size() - failed << " of " << entries.size() << " rendered\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string command = argc >= 2 ? argv[1] : "";
    try {
        if (command == "render") {
            bool allowFallback = false;
            const Fixture fixture = parse_render(argc, argv, allowFallback);
            std::optional<nm::gpu::DeviceResult> created = open_device(allowFallback);
            if (!created) return 1;
            return render_fixture(created->device, fixture);
        }
        if (command == "batch" && argc >= 3) {
            bool allowFallback = false;
            for (int i = 3; i < argc; ++i) {
                if (std::string(argv[i]) == "--allow-fallback") allowFallback = true;
                else throw Usage(std::string("unknown option ") + argv[i]);
            }
            return run_batch(argv[2], allowFallback);
        }
    } catch (const Usage& e) {
        std::cerr << "nm-render: " << e.what() << "\n\n";
    }
    std::cerr << kUsage;
    return 2;
}
