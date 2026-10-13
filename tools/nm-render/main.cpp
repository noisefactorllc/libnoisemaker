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
// names of rust-gpu's nm-render manifest, "obj", and the timed members
// below; a fixture with any other member fails, as nm-render does not
// support it yet.
//
// Meshes follow the minter's plan: when a pass reads a mesh texture or the
// fixture has an OBJ (--obj, "obj"), each externalMesh effect's first
// built-in mesh (read from $NM_REFERENCE_ROOT/<path>, the files the demo page
// fetches through the reference's shaders/share link) loads into its mesh,
// then the OBJ into mesh0, or an empty
// mesh0 when there is neither; each is packed into 256 x 256 textures and
// uploaded with the backend's uploadMeshData, as the page's renderer does.
//
// A timed fixture (--run-seconds S --sample-every E, --run-frames N,
// --sample-every-frames K, --sample-frames a,b,c; the manifest's runSeconds,
// sampleEvery, runFrames, sampleEveryFrames and sampleFrames) skips the
// pinned clock: after the same fresh-state reset it steps render(t) with
// t = ((frame + 1) / 600) % 1 and writes <out stem>.t<sec>.png every E
// seconds and <out stem>.f<frames>.png every K frames and after each listed
// frame count, the minter's sample schedule.
//
// A --dsl program gets the demo page's step values after it compiles: the
// reference ProgramState's values for it, applied with the renderer's
// applyStepParameterValues, with a fresh page's enum tree. The page's
// control initialization, which can coerce some of them first, is not
// ported yet.
//
// A MIDI sidecar (--midi FILE, the manifest's "midi", or for a --dsl program
// <name>.midi.json beside it: {"messages": [[status, data1, data2], ...]})
// connects a MIDI state to the pipeline and delivers those raw messages
// after the program loads, as the minter does through the page's renderer.
//
// Not yet: Portable effects, natively produced host inputs and texture
// dumps.

#include "core/graph/dsl_compiler.h"
#include "core/lang/effect_registry.h"
#include "core/edit/program_state/host.h"
#include "core/edit/program_state/program_state.h"
#include "core/value/js_number.h"
#include "core/value/json.h"
#include "gpu/backend/backend.h"
#include "gpu/backend/pipeline.h"
#include "gpu/dawn_device.h"
#include "gpu/device.h"
#include "host/midi_state.h"
#include "host/obj_parser.h"
#include "png.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
    "                        [--time T] [--frames N] [--host-texture ID=PNG ...] [--obj FILE] [--midi FILE]\n"
    "                        [--allow-fallback]\n"
    "                        [--run-seconds S --sample-every E] [--run-frames N] [--sample-every-frames K]\n"
    "                        [--sample-frames a,b,c]\n"
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
    std::string obj;
    std::string midi;
    // A timed run (batch-golden.mjs isTimed and sampleSchedule).
    double runSeconds = 0;
    double sampleEvery = 5;
    int runFrames = 0;
    int sampleEveryFrames = 0;
    std::vector<int> sampleFrames;
    bool timed() const { return runSeconds > 0 || runFrames > 0 || !sampleFrames.empty(); }
};

struct Sample {
    int frame;
    std::vector<std::string> labels;
};

// sampleSchedule: [{ frame, labels }] in frame order.
std::vector<Sample> sample_schedule(const Fixture& fixture) {
    std::vector<Sample> samples;
    const auto add = [&](int frame, const std::string& label) {
        if (frame <= 0) return;
        for (Sample& s : samples) {
            if (s.frame != frame) continue;
            if (std::find(s.labels.begin(), s.labels.end(), label) == s.labels.end()) s.labels.push_back(label);
            return;
        }
        samples.push_back(Sample{frame, {label}});
    };
    int total = fixture.runFrames;
    if (fixture.runSeconds > 0) {
        // Math.max(1, Math.round(sampleEvery * 60)), Math.floor(runSeconds * 60 / everyFrames)
        const int everyFrames = int(std::max(1.0, std::floor(fixture.sampleEvery * 60 + 0.5)));
        const int count = int(std::max(1.0, std::floor(fixture.runSeconds * 60 / everyFrames)));
        for (int k = 0; k < count; ++k) {
            add((k + 1) * everyFrames, "t" + nm::js_to_utf8(nm::js_number_to_string((k + 1) * fixture.sampleEvery)));
        }
        total = std::max(total, count * everyFrames);
    }
    if (fixture.sampleEveryFrames > 0) {
        for (int f = fixture.sampleEveryFrames; f <= total; f += fixture.sampleEveryFrames) add(f, "f" + std::to_string(f));
    }
    for (const int f : fixture.sampleFrames) add(f, "f" + std::to_string(f));
    std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) { return a.frame < b.frame; });
    return samples;
}

// out without its .png, then .<label>.png.
std::string sample_path(const std::string& out, const std::string& label) {
    const std::string stem = out.size() > 4 && out.compare(out.size() - 4, 4, ".png") == 0 ? out.substr(0, out.size() - 4) : out;
    return stem + "." + label + ".png";
}

struct Usage : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The whole file, or false when it cannot be opened or read (a directory,
// an I/O error).
bool read_file(const std::string& path, std::string& text) {
    try {
        const std::vector<uint8_t> bytes = nm_render::read_file(path);
        text.assign(bytes.begin(), bytes.end());
        return true;
    } catch (const std::runtime_error&) {
        return false;
    }
}

// An environment variable, or empty; MSVC's /W4 refuses std::getenv.
std::string environment(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
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
        } else if (arg == "--obj") {
            fixture.obj = v;
        } else if (arg == "--midi") {
            fixture.midi = v;
        } else if (arg == "--sample-frames") {
            std::string list = v;
            size_t start = 0;
            while (start <= list.size()) {
                const size_t comma = list.find(',', start);
                const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                const std::optional<double> n = number(item.c_str());
                // Frame counts of 0 or less are ignored, as sampleSchedule's add ignores them.
                if (!n || *n != std::floor(*n) || *n < -1e6 || *n > 1e6) throw Usage("--sample-frames takes whole frame counts");
                fixture.sampleFrames.push_back(int(*n));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
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
            else if (arg == "--run-seconds" && *n >= 0 && *n <= 3600) fixture.runSeconds = *n;
            else if (arg == "--sample-every" && *n >= 0 && *n <= 3600) fixture.sampleEvery = *n;
            else if (arg == "--run-frames" && *n == std::floor(*n) && *n >= 0 && *n <= 1e6) fixture.runFrames = int(*n);
            else if (arg == "--sample-every-frames" && *n == std::floor(*n) && *n >= 0 && *n <= 1e6) fixture.sampleEveryFrames = int(*n);
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
                                              u"hostTextures", u"obj", u"midi", u"runSeconds", u"sampleEvery", u"runFrames",
                                              u"sampleEveryFrames", u"sampleFrames"};
        bool known = false;
        for (const char16_t* name : supported) known = known || key == name;
        if (!known) throw Usage("unsupported member " + nm::js_to_utf8(key));
    }
    Fixture fixture;
    fixture.graph = string_member(entry, u"graph");
    fixture.dsl = string_member(entry, u"dsl");
    fixture.out = string_member(entry, u"out");
    fixture.obj = string_member(entry, u"obj");
    fixture.midi = string_member(entry, u"midi");
    if (const auto size = number_member(entry, u"size")) fixture.width = fixture.height = *size;
    if (const auto width = number_member(entry, u"width")) fixture.width = *width;
    if (const auto height = number_member(entry, u"height")) fixture.height = *height;
    if (const auto time = number_member(entry, u"time")) fixture.time = *time;
    if (const auto frames = number_member(entry, u"frames")) {
        if (*frames != std::floor(*frames) || *frames < 0 || *frames > 1e6) throw Usage("frames must be a whole count");
        fixture.frames = int(*frames);
    }
    const auto whole = [&](const char16_t* key, int& out) {
        if (const auto n = number_member(entry, key)) {
            if (*n != std::floor(*n) || *n < 0 || *n > 1e6) throw Usage(nm::js_to_utf8(key) + " must be a whole count");
            out = int(*n);
        }
    };
    if (const auto n = number_member(entry, u"runSeconds")) {
        if (*n < 0 || *n > 3600) throw Usage("runSeconds must be from 0 to 3600");
        fixture.runSeconds = *n;
    }
    if (const auto n = number_member(entry, u"sampleEvery")) {
        if (*n < 0 || *n > 3600) throw Usage("sampleEvery must be from 0 to 3600");
        fixture.sampleEvery = *n;
    }
    whole(u"runFrames", fixture.runFrames);
    whole(u"sampleEveryFrames", fixture.sampleEveryFrames);
    if (const nm::Value* frames = entry.find(u"sampleFrames")) {
        if (!frames->is_array()) throw Usage("sampleFrames must be an array");
        for (const nm::Value& f : frames->as_array()) {
            if (!f.is_number() || f.as_number() != std::floor(f.as_number()) || f.as_number() < -1e6 || f.as_number() > 1e6) {
                throw Usage("sampleFrames holds whole frame counts");
            }
            fixture.sampleFrames.push_back(int(f.as_number()));
        }
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

nm::EffectRegistry& registry() {
    static nm::EffectRegistry instance;
    static const bool loaded = (instance.loadEmbedded(), true);
    (void)loaded;
    return instance;
}

// The fixture's graph, and its DSL text when it has one.
nm::Value load_graph(const Fixture& fixture, std::string& dslText) {
    std::string text;
    const std::string& path = fixture.dsl.empty() ? fixture.graph : fixture.dsl;
    if (!read_file(path, text)) throw std::runtime_error("cannot read " + path);
    if (!fixture.graph.empty()) return nm::json::parse(text);
    dslText = text;
    return nm::Value(nm::compileGraphJson(nm::utf8_to_js(text), registry()));
}

// Part of what the demo page does after compiling a program
// (rebuildPipelineFromDsl): the UI loads it into ProgramState (fromDsl) and
// the renderer applies the state's step values to the pipeline
// (applyStepParameterValues). The UI's control initialization between the
// two, which can coerce some values, is not ported; the enum tree is a fresh
// page's, with the program's effects' choices (renderer_enum_tree).
void apply_demo_step_values(be::Pipeline& pipeline, const std::string& dslText) {
    nm::ProgramState<> state(registry());
    state.from_dsl(nm::utf8_to_js(dslText));
    // A fresh page's enum tree: the standard enums and the choices of the
    // program's effects, which the page loads before it compiles. A minting
    // session's page may also hold the choices of programs it loaded before.
    std::vector<nm::JsString> effects;
    const nm::Value* passes = pipeline.graph().is_object() ? pipeline.graph().as_object().find(u"passes") : nullptr;
    if (passes && passes->is_array()) {
        for (const nm::Value& pass : passes->as_array()) {
            const nm::Value key = pass.is_object() ? nm::jsv::member(pass, u"effectKey") : nm::Value();
            if (key.is_string() && std::find(effects.begin(), effects.end(), key.as_string()) == effects.end()) {
                effects.push_back(key.as_string());
            }
        }
    }
    const nm::Value enums = nm::renderer_enum_tree(registry(), &effects);
    pipeline.applyStepParameterValues(
        nm::Value(state.get_all_step_values()), [](const nm::JsString& key) { return registry().getEffectValue(key); },
        [&enums](const nm::Value& value, const nm::Value& spec) { return nm::convert_parameter_for_uniform(value, spec, enums); });
}

// /^global_mesh\d+_(positions|normals|uvs)/, the minter's mesh input test.
bool reads_mesh(const nm::JsString& id) {
    const nm::JsString prefix = u"global_mesh";
    if (id.rfind(prefix, 0) != 0) return false;
    size_t i = prefix.size();
    while (i < id.size() && id[i] >= u'0' && id[i] <= u'9') ++i;
    if (i == prefix.size() || i >= id.size() || id[i] != u'_') return false;
    for (const nm::JsString name : {u"positions", u"normals", u"uvs"}) {
        if (id.compare(i + 1, name.size(), name) == 0) return true;
    }
    return false;
}

// _packCacheAndUploadMesh: 256 x 256 textures.
void upload_mesh(be::WebGPUBackend& backend, const nm::JsString& meshId, const nm::ObjMeshData& mesh) {
    const nm::PackedMeshData packed = nm::packMeshDataForTextures(mesh, 256, 256);
    backend.uploadMeshData(meshId, packed.positionData, packed.normalData, packed.uvData, nm::Value(256.0),
                           nm::Value(256.0), nm::Value(double(packed.vertexCount)));
}

// The minter's MIDI sidecar: the fixture's file, else for a DSL program its
// <name>.midi.json; the raw messages delivered to a MIDI state the pipeline
// reads (CanvasRenderer.setMidiState, MidiState.handleMessage).
void deliver_midi(be::Pipeline& pipeline, const Fixture& fixture) {
    std::string path = fixture.midi;
    if (path.empty() && !fixture.dsl.empty()) {
        const std::string sidecar = fixture.dsl.size() > 4 && fixture.dsl.compare(fixture.dsl.size() - 4, 4, ".dsl") == 0
                                        ? fixture.dsl.substr(0, fixture.dsl.size() - 4) + ".midi.json"
                                        : std::string();
        // existsSync, then a read that must succeed, as the minter does.
        std::error_code error;
        if (!sidecar.empty() && std::filesystem::exists(sidecar, error)) path = sidecar;
    }
    if (path.empty()) return;
    std::string text;
    if (!read_file(path, text)) throw std::runtime_error("cannot read " + path);
    const nm::Value sidecar = nm::json::parse(text);
    const nm::Value* messages = sidecar.is_object() ? sidecar.as_object().find(u"messages") : nullptr;
    if (!messages || !messages->is_array()) throw std::runtime_error(path + ": \"messages\" must be an array of byte arrays");
    std::vector<std::vector<uint8_t>> raw;
    for (const nm::Value& message : messages->as_array()) {
        if (!message.is_array() || message.as_array().empty()) throw std::runtime_error(path + ": a message must be a non-empty byte array");
        std::vector<uint8_t> bytes;
        for (const nm::Value& byte : message.as_array()) {
            if (!byte.is_number() || byte.as_number() != std::floor(byte.as_number()) || byte.as_number() < 0 || byte.as_number() > 255) {
                throw std::runtime_error(path + ": message bytes must be whole numbers from 0 to 255");
            }
            bytes.push_back(uint8_t(byte.as_number()));
        }
        raw.push_back(std::move(bytes));
    }
    // new MidiState(): the page's default, with a port registry.
    auto state = std::make_shared<nm::MidiState>();
    pipeline.setMidiState(state);
    for (const std::vector<uint8_t>& bytes : raw) state->handleMessage(bytes.data(), std::ptrdiff_t(bytes.size()));
}

// The minter's meshPlan and applyMeshPlan (batch-golden.mjs).
void load_meshes(const nm::Value& graph, const Fixture& fixture, be::WebGPUBackend& backend) {
    std::optional<std::string> objText;
    if (!fixture.obj.empty()) {
        std::string text;
        if (!read_file(fixture.obj, text)) throw std::runtime_error("cannot read " + fixture.obj);
        objText = std::move(text);
    }
    const nm::Value* passes = graph.is_object() ? graph.as_object().find(u"passes") : nullptr;
    if (!passes || !passes->is_array()) return;
    bool readsMesh = false;
    for (const nm::Value& pass : passes->as_array()) {
        const nm::Value* inputs = pass.is_object() ? pass.as_object().find(u"inputs") : nullptr;
        if (!inputs || !inputs->is_object()) continue;
        for (const auto& key : inputs->as_object().keys()) {
            const nm::Value* id = inputs->as_object().find(key);
            if (id && id->is_string() && reads_mesh(id->as_string())) readsMesh = true;
        }
    }
    if (!readsMesh && !objText) return;
    std::vector<std::pair<nm::JsString, std::string>> builtins;
    std::vector<nm::JsString> seen;
    for (const nm::Value& pass : passes->as_array()) {
        if (!pass.is_object()) continue;
        const nm::Value* key = pass.as_object().find(u"effectKey");
        if (!key || !key->is_string() || key->as_string().empty()) continue;
        const nm::Value* step = pass.as_object().find(u"stepIndex");
        const nm::JsString seenKey = (step ? nm::jsv::to_string(*step) : nm::JsString(u"undefined")) + u"|" + key->as_string();
        if (std::find(seen.begin(), seen.end(), seenKey) != seen.end()) continue;
        seen.push_back(seenKey);
        const nm::Value def = registry().getEffectValue(key->as_string());
        if (!def.is_object()) continue;
        const nm::Value* meshId = def.as_object().find(u"externalMesh");
        const nm::Value* meshes = def.as_object().find(u"builtinMeshes");
        if (!meshId || !meshId->is_string() || !meshes) continue;
        // The catalog lists builtinMeshes as [{ name, path }] in the
        // definition's order; the minter takes the first.
        const nm::Value* first = meshes->is_array() && !meshes->as_array().empty() ? &meshes->as_array()[0] : nullptr;
        const nm::Value* path = first && first->is_object() ? first->as_object().find(u"path") : nullptr;
        if (path && path->is_string()) builtins.emplace_back(meshId->as_string(), nm::js_to_utf8(path->as_string()));
    }
    for (const auto& [meshId, path] : builtins) {
        // The demo page fetches `${shaderBasePath}/${path}` from the
        // reference's shaders directory, whose share is a link to ../share:
        // the files read here, without depending on the link (a checkout
        // without symbolic links has a text file there).
        const std::string root = environment("NM_REFERENCE_ROOT");
        if (root.empty()) throw std::runtime_error("a built-in mesh (" + path + ") needs NM_REFERENCE_ROOT");
        upload_mesh(backend, meshId, nm::loadOBJ(root + "/" + path));
    }
    if (objText) {
        upload_mesh(backend, u"mesh0", nm::parseOBJ(nm::utf8_to_js(*objText)));
    } else if (builtins.empty()) {
        upload_mesh(backend, u"mesh0", nm::parseOBJ(u""));
    }
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
            std::string dslText;
            const nm::Value graph = load_graph(fixture, dslText);
            be::Pipeline pipeline(graph, backend, {});
            pipeline.init(fixture.width, fixture.height);
            if (!fixture.dsl.empty()) apply_demo_step_values(pipeline, dslText);
            load_meshes(graph, fixture, backend);
            deliver_midi(pipeline, fixture);
            // The minter's captures: texture contents, uploaded with flipY false.
            std::vector<nm::JsString> keep;
            for (const auto& [id, path] : fixture.hostTextures) {
                const nm_render::Image image = nm_render::decode_png(nm_render::read_file(path));
                keep.push_back(nm::utf8_to_js(id));
                backend.updateTextureFromPixels(keep.back(), image.width, image.height, image.rgba, false);
            }
            reset_to_fresh_state(pipeline, backend, keep);
            // The capture: the render surface's read texture, as readPixels converts it.
            const auto capture = [&]() {
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
                return nm_render::encode_png_rgba8(width, height, pixels.data);
            };
            if (fixture.timed()) {
                int frame = 0;
                for (const Sample& sample : sample_schedule(fixture)) {
                    for (; frame < sample.frame; ++frame) {
                        pipeline.render(std::fmod((frame + 1) / 600.0, 1.0));
                        dawn.processEvents();
                    }
                    const std::vector<uint8_t> png = capture();
                    for (const std::string& label : sample.labels) {
                        if (!nm_render::write_file(sample_path(fixture.out, label), png)) status = fail("cannot write " + label);
                    }
                }
            } else {
                for (int i = 0; i < fixture.frames; ++i) {
                    pipeline.render(fixture.time);
                    dawn.processEvents();
                }
                if (!nm_render::write_file(fixture.out, capture())) status = fail("cannot write the PNG");
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
        bool lost = false;
        {
            std::lock_guard<std::mutex> lock(created->device.errors->mutex);
            lost = created->device.errors->lost;
        }
        if (lost) {
            // Every later fixture would fail for this one's loss.
            std::cerr << "nm-render: batch stopped after entry " << i << ": the device was lost; " << entries.size() - i - 1
                      << " entries not rendered\n";
            failed += entries.size() - i - 1;
            break;
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
