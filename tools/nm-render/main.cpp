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
// names of rust-gpu's nm-render manifest, "obj", "midi", "portable", and
// the timed members below; a fixture with any other member fails, as
// nm-render does not support it yet.
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
// reference ProgramState's values for it, coerced as the page's control
// initialization sets them, applied with the renderer's
// applyStepParameterValues, with a fresh page's enum tree.
//
// A MIDI sidecar (--midi FILE, the manifest's "midi", or for a --dsl program
// <name>.midi.json beside it: {"messages": [[status, data1, data2], ...]})
// connects a MIDI state to the pipeline and delivers those raw messages
// after the program loads, as the minter does through the page's renderer.
//
// A --dsl program's media steps that have passes in the graph get the demo
// page's default image, $NM_REFERENCE_ROOT/demo/shaders/img/testcard.png, or
// the host texture given for their texture, with the page's imageSize. (The
// page also loads the image of a skipped media step, which renders
// nothing.) The page uploads an image with copyExternalImageToTexture;
// nm-render writes its decoded pixels.
//
// A --dsl program's asyncInit overlays (filter/fibers, filter/scratches,
// filter/strayHair) are drawn natively (host/async_overlays.h) with the
// parameters the page's last draw has; a host texture given for
// <nodeId>_overlayTex is used instead, with no trace.
//
// A Portable sidecar (the manifest's "portable", or for a --dsl program
// <name>.portable.json beside it), with its shaders loaded by the rule of
// tools/portable.mjs, registers its user effect before the program
// compiles, as the minter does through registerPortableEffect. A sidecar
// that fails that method's checks registers nothing.
//
// Not yet: text produced natively, and texture dumps.

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
#include "gpu/backend/automation.h"
#include "host/async_overlays.h"
#include "host/midi_state.h"
#include "host/obj_parser.h"
#include "host/stroke_canvas.h"
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
    std::string portable;
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
                                              u"hostTextures", u"obj", u"midi", u"portable", u"runSeconds", u"sampleEvery",
                                              u"runFrames", u"sampleEveryFrames", u"sampleFrames"};
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
    fixture.portable = string_member(entry, u"portable");
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

// The whitespace String.prototype.trim removes.
bool js_space(char16_t c) {
    return c == u' ' || (c >= u'\t' && c <= u'\r') || c == 0x00A0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

// typeof source === 'string' && source.trim().length > 0
bool has_source(const nm::Value& source) {
    return source.is_string() && std::any_of(source.as_string().begin(), source.as_string().end(), [](char16_t c) { return !js_space(c); });
}

// /^[a-zA-Z_][a-zA-Z0-9_]*$/
bool valid_identifier(const nm::JsString& name) {
    const auto word = [](char16_t c) { return c == u'_' || (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); };
    if (name.empty() || !word(name[0])) return false;
    return std::all_of(name.begin(), name.end(), [&](char16_t c) { return word(c) || (c >= u'0' && c <= u'9'); });
}

// canvas.js sanitizeEnumName: the letter after each whitespace run upper
// cased, whitespace and other non-identifier characters dropped; empty when
// the result is not an identifier.
nm::JsString sanitize_enum_name(const nm::JsString& name) {
    nm::JsString joined;
    for (size_t i = 0; i < name.size(); ++i) {
        if (!js_space(name[i])) {
            joined += name[i];
            continue;
        }
        while (i < name.size() && js_space(name[i])) ++i;
        if (i < name.size()) joined += name[i] >= u'a' && name[i] <= u'z' ? char16_t(name[i] - 32) : name[i];
    }
    nm::JsString out;
    for (const char16_t c : joined) {
        if (c == u'_' || (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9')) out += c;
    }
    return valid_identifier(out) ? out : nm::JsString();
}

// tools/portable.mjs loadPortableDefinition: the sidecar <name>.portable.json
// with each pass program P's <name>.P.wgsl and <name>.P.glsl beside it
// attached as shaders[P].wgsl and shaders[P].glsl, unless the definition
// carries that source.
nm::Value load_portable_definition(const std::string& path) {
    std::string text;
    if (!read_file(path, text)) throw std::runtime_error("cannot read " + path);
    nm::Value def = nm::json::parse(text);
    const std::filesystem::path file(path);
    std::string name = file.filename().string();
    const std::string suffix = ".portable.json";
    if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        name.resize(name.size() - suffix.size());
    }
    if (!def.is_object()) return def;
    const nm::Value* passes = def.as_object().find(u"passes");
    if (!passes || !passes->is_array()) return def;
    std::vector<nm::JsString> programs;
    for (const nm::Value& pass : passes->as_array()) {
        const nm::Value program = nm::jsv::member(pass, u"program");
        if (program.is_string()) programs.push_back(program.as_string());
    }
    for (const nm::JsString& program : programs) {
        for (const char16_t* language : {u"wgsl", u"glsl"}) {
            const std::filesystem::path source = file.parent_path() / (name + "." + nm::js_to_utf8(program) + "." + nm::js_to_utf8(language));
            std::error_code error;
            if (!std::filesystem::exists(source, error)) continue;
            nm::Object& object = def.as_object();
            nm::Value& shaders = object[u"shaders"];
            if (!shaders.is_object() && !shaders.is_array()) shaders = nm::Value(nm::Object{});
            if (!shaders.is_object()) continue;
            nm::Value& entry = shaders.as_object()[program];
            if (!entry.is_object() && !entry.is_array()) entry = nm::Value(nm::Object{});
            if (!entry.is_object() || entry.as_object().find(language)) continue;
            std::string bytes;
            if (!read_file(source.string(), bytes)) throw std::runtime_error("cannot read " + source.string());
            entry.as_object().set(language, nm::Value(nm::utf8_to_js(bytes)));
        }
    }
    return def;
}

// The checks of CanvasRenderer.registerPortableEffect, without its error
// messages: a definition that fails one is not registered, and a program
// that calls its effect fails to compile.
bool portable_valid(const nm::Value& def, const nm::JsString& func) {
    if (!def.is_object() || !valid_identifier(func)) return false;
    // Object.getOwnPropertyNames(Object.prototype) and 'prototype'.
    static const char16_t* reserved[] = {u"constructor", u"__defineGetter__", u"__defineSetter__", u"hasOwnProperty",
                                         u"__lookupGetter__", u"__lookupSetter__", u"isPrototypeOf", u"propertyIsEnumerable",
                                         u"toString", u"valueOf", u"__proto__", u"toLocaleString", u"prototype"};
    const auto is_reserved = [&](const nm::JsString& key) {
        return std::any_of(std::begin(reserved), std::end(reserved), [&](const char16_t* word) { return key == word; });
    };
    if (is_reserved(func)) return false;
    std::vector<const nm::Value*> pending{&def};
    while (!pending.empty()) {
        const nm::Value* value = pending.back();
        pending.pop_back();
        if (value->is_object()) {
            for (const nm::JsString& key : value->as_object().keys()) {
                if (is_reserved(key)) return false;
                pending.push_back(value->as_object().find(key));
            }
        } else if (value->is_array()) {
            for (const nm::Value& child : value->as_array()) pending.push_back(&child);
        }
    }
    const nm::Object& object = def.as_object();
    const auto field = [&](const char16_t* key) { const nm::Value* v = object.find(key); return v ? *v : nm::Value(); };
    const nm::Value ns = field(u"namespace");
    if (!ns.is_undefined() && !nm::jsv::strict_equals(ns, nm::Value(u"user"))) return false;
    const nm::Value starter = field(u"starter");
    if (!starter.is_undefined() && !starter.is_bool()) return false;
    const nm::Value passes = field(u"passes");
    if (!passes.is_array() || passes.as_array().empty()) return false;
    const nm::Value shaders = field(u"shaders");
    if (!shaders.is_object()) return false;
    for (const nm::Value& pass : passes.as_array()) {
        if (!pass.is_object()) return false;
        const nm::Value program = nm::jsv::member(pass, u"program");
        if (!program.is_string() || program.as_string().empty()) return false;
        for (const char16_t* key : {u"inputs", u"outputs"}) {
            const nm::Value textures = nm::jsv::member(pass, key);
            if (textures.is_undefined()) continue;
            if (!textures.is_object()) return false;
            for (const auto& [name, reference] : nm::jsv::entries(textures)) {
                if (!has_source(reference)) return false;
            }
        }
        const nm::Value source = nm::jsv::member(shaders, program.as_string());
        if (!source.is_object() || !(has_source(nm::jsv::member(source, u"glsl")) || has_source(nm::jsv::member(source, u"wgsl")))) {
            return false;
        }
    }
    for (const char16_t* language : {u"glsl", u"wgsl"}) {
        const auto present = [&](const nm::Value& pass) {
            return has_source(nm::jsv::member(nm::jsv::member(shaders, nm::jsv::member(pass, u"program").as_string()), language));
        };
        if (std::any_of(passes.as_array().begin(), passes.as_array().end(), present) &&
            !std::all_of(passes.as_array().begin(), passes.as_array().end(), present)) {
            return false;
        }
    }
    const nm::Value globals = field(u"globals");
    if (!globals.is_undefined()) {
        if (!globals.is_object()) return false;
        for (const auto& [key, spec] : nm::jsv::entries(globals)) {
            if (!spec.is_object()) return false;
            const nm::Value choices = nm::jsv::member(spec, u"choices");
            if (choices.is_undefined()) continue;
            if (!choices.is_object()) return false;
            const bool strings = nm::jsv::strict_equals(nm::jsv::member(spec, u"type"), nm::Value(u"string"));
            for (const auto& [name, value] : nm::jsv::entries(choices)) {
                if (value.is_null()) continue;
                if (strings ? !value.is_string() : !nm::jsv::is_finite_number(value)) return false;
            }
        }
    }
    if (object.find(u"paramAliases")) return false;
    return !registry().hasEffect(nm::JsText(u"user." + func)) && !registry().hasEffect(nm::JsText(u"user/" + func));
}

// CanvasRenderer.registerPortableEffect, as the minter calls it once per
// session: an effect already registered as user/<func> is kept. The
// Effect instance takes the definition's fields, in namespace user, and is
// registered under user.<func> and user/<func> (registerEffectWithRuntime
// also registers the bare name, then restores the bare lookup it found),
// with its operator, its choices as enums and, for a starter, the starter
// op user.<func>.
void register_portable_effect(const nm::Value& definition) {
    nm::Value funcValue = nm::jsv::member(definition, u"func");
    if (funcValue.is_undefined() || funcValue.is_null()) funcValue = nm::jsv::member(definition, u"name");
    const nm::JsString func = funcValue.is_string() ? funcValue.as_string() : nm::JsString();
    if (definition.is_object() && valid_identifier(func) && registry().hasEffect(nm::JsText(u"user/" + func))) return;
    if (!portable_valid(definition, func)) return;
    const nm::Object& source = definition.as_object();
    nm::Object instance;
    const auto copy = [&](const char16_t* key) {
        const nm::Value* value = source.find(key);
        if (value && nm::jsv::truthy(*value)) instance.set(key, *value);
    };
    copy(u"name");
    instance.set(u"namespace", nm::Value(u"user"));
    instance.set(u"func", nm::Value(func));
    for (const char16_t* key : {u"description", u"tags", u"globals", u"passes", u"textures", u"textures3d", u"externalTexture",
                                u"externalMesh", u"builtinMeshes", u"outputTex3d", u"outputGeo", u"uniformLayout", u"uniformLayouts",
                                u"openCategories", u"defaultProgram"}) {
        copy(key);
    }
    if (const nm::Value* hidden = source.find(u"hidden"); hidden && nm::jsv::truthy(*hidden)) instance.set(u"hidden", nm::Value(true));
    copy(u"deprecatedBy");
    instance.set(u"shaders", *source.find(u"shaders"));
    static const char16_t* pipelineInputs[] = {u"inputTex", u"inputTex3d", u"inputGeo", u"inputXyz", u"inputVel", u"inputRgba", u"src",
                                               u"o0", u"o1", u"o2", u"o3", u"o4", u"o5", u"o6", u"o7"};
    bool starter = true;
    if (const nm::Value* given = source.find(u"starter")) {
        starter = given->as_bool();
    } else {
        for (const nm::Value& pass : source.find(u"passes")->as_array()) {
            for (const auto& [name, input] : nm::jsv::entries(nm::jsv::member(pass, u"inputs"))) {
                for (const char16_t* id : pipelineInputs) starter = starter && !(input.is_string() && input.as_string() == id);
            }
        }
    }
    instance.set(u"starter", nm::Value(starter));
    const nm::Value effect(std::move(instance));
    const nm::JsString opName = u"user." + func;
    const auto setup = [](std::initializer_list<std::pair<const char16_t*, nm::Value>> fields) {
        nm::Object action;
        for (const auto& [key, value] : fields) action.set(key, value);
        registry().applySetup(nm::Value(std::move(action)));
    };
    setup({{u"op", nm::Value(u"registerEffect")}, {u"name", nm::Value(opName)}, {u"definition", effect}});
    setup({{u"op", nm::Value(u"registerEffect")}, {u"name", nm::Value(u"user/" + func)}, {u"definition", effect}});
    // The operator's arguments, and the choices of those without an enum.
    nm::Array args;
    nm::Object choices;
    for (const auto& [key, spec] : nm::jsv::entries(nm::jsv::member(effect, u"globals"))) {
        nm::Value enumPath = nm::jsv::member(spec, u"enum");
        if (!nm::jsv::truthy(enumPath)) enumPath = nm::jsv::member(spec, u"enumPath");
        const nm::Value specChoices = nm::jsv::member(spec, u"choices");
        if (nm::jsv::truthy(specChoices) && !nm::jsv::truthy(enumPath)) {
            enumPath = nm::Value(opName + u"." + key);
            nm::Object entries;
            for (const auto& [name, value] : nm::jsv::entries(specChoices)) {
                if (!name.empty() && name.back() == u':') continue;
                nm::Object leaf;
                leaf.set(u"type", nm::Value(u"Number"));
                leaf.set(u"value", value);
                entries.set(name, nm::Value(leaf));
                const nm::JsString sanitized = sanitize_enum_name(name);
                if (!sanitized.empty() && sanitized != name) entries.set(sanitized, nm::Value(leaf));
            }
            nm::Value& table = choices[u"user"];
            if (!table.is_object()) table = nm::Value(nm::Object{});
            nm::Value& funcTable = table.as_object()[func];
            if (!funcTable.is_object()) funcTable = nm::Value(nm::Object{});
            funcTable.as_object().set(key, nm::Value(std::move(entries)));
        }
        nm::Object arg;
        arg.set(u"name", nm::Value(key));
        const nm::Value type = nm::jsv::member(spec, u"type");
        if (!type.is_undefined()) arg.set(u"type", nm::jsv::strict_equals(type, nm::Value(u"vec4")) ? nm::Value(u"color") : type);
        for (const auto& [name, value] : {std::pair<const char16_t*, nm::Value>{u"default", nm::jsv::member(spec, u"default")},
                                          {u"enum", enumPath}, {u"enumPath", enumPath}, {u"min", nm::jsv::member(spec, u"min")},
                                          {u"max", nm::jsv::member(spec, u"max")}, {u"uniform", nm::jsv::member(spec, u"uniform")},
                                          {u"choices", specChoices}}) {
            if (!value.is_undefined()) arg.set(name, value);
        }
        args.push_back(nm::Value(std::move(arg)));
    }
    nm::Object opSpec;
    opSpec.set(u"name", nm::Value(func));
    opSpec.set(u"args", nm::Value(std::move(args)));
    setup({{u"op", nm::Value(u"registerOp")}, {u"name", nm::Value(opName)}, {u"spec", nm::Value(std::move(opSpec))}});
    if (nm::jsv::truthy(nm::jsv::member(effect, u"hidden")) && nm::jsv::truthy(nm::jsv::member(effect, u"deprecatedBy"))) {
        setup({{u"op", nm::Value(u"registerEffectAlias")}, {u"opName", nm::Value(opName)}, {u"newName", nm::jsv::member(effect, u"deprecatedBy")}});
    }
    setup({{u"op", nm::Value(u"mergeIntoEnums")}, {u"source", nm::Value(std::move(choices))}});
    if (starter) setup({{u"op", nm::Value(u"registerStarterOps")}, {u"names", nm::Value(nm::Array{nm::Value(opName)})}});
}

// The fixture's Portable sidecar: its "portable" member, else for a DSL
// program its <name>.portable.json, as the minter finds it (portableSidecar).
std::string portable_sidecar(const Fixture& fixture) {
    if (!fixture.portable.empty() || fixture.dsl.empty()) return fixture.portable;
    const std::string& dsl = fixture.dsl;
    if (dsl.size() <= 4 || dsl.compare(dsl.size() - 4, 4, ".dsl") != 0) return {};
    const std::string sidecar = dsl.substr(0, dsl.size() - 4) + ".portable.json";
    std::error_code error;
    return std::filesystem::exists(sidecar, error) ? sidecar : std::string();
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

// "Oscillator", "Midi" or "Audio": the type of an automation config.
bool automation_kind(const nm::Value& type) {
    return type.is_string() && (type.as_string() == u"Oscillator" || type.as_string() == u"Midi" || type.as_string() == u"Audio");
}

// An automation config: value.type or value._ast.type is an automation kind.
bool automation_type(const nm::Value& value) {
    return automation_kind(nm::jsv::member(value, u"type")) ||
           automation_kind(nm::jsv::member(nm::jsv::member(value, u"_ast"), u"type"));
}

// value && typeof value === "object"
bool object_value(const nm::Value& value) { return nm::jsv::is_object_like(value); }

// The parameter values the demo page writes when it builds a program's
// controls (DemoUI.createEffectControlsFromState): for each effect step with
// globals, each control's _createControlGroup takes the state's value, else
// the step's argument, else the default; it leaves an automated parameter
// (an oscillator, MIDI or audio binding) as it is, binding a variable
// reference as { _varRef }, and sets every other value through ProgramState's
// setValue, which coerces it to the parameter's type (a function or state
// value of a numeric parameter becomes its default). A member control then
// stores the enum path that matches the value, or its first choice.
void initialize_demo_controls(nm::ProgramState<>& state, const nm::Value& enums) {
    const nm::Value structure = state.get_structure();
    if (!structure.is_array() || structure.as_array().empty()) return;
    if (!nm::jsv::truthy(nm::jsv::member(state.get_compiled(), u"plans"))) return;
    for (const nm::Value& info : structure.as_array()) {
        const nm::Value effectKey = nm::jsv::member(info, u"effectKey");
        const nm::Value name = nm::jsv::member(info, u"name");
        const nm::Value ns = nm::jsv::member(info, u"namespace");
        if (effectKey.is_string() && (effectKey.as_string() == u"_write" || effectKey.as_string() == u"_read" ||
                                      effectKey.as_string() == u"_read3d" || effectKey.as_string() == u"_write3d")) {
            continue;
        }
        // getEffect(effectKey), then getEffect(`${namespace}.${name}`), then getEffect(name).
        const auto lookup = [](const nm::Value& key) {
            return nm::jsv::truthy(key) ? registry().getEffectValue(nm::jsv::to_string(key)) : nm::Value();
        };
        nm::Value def = lookup(effectKey);
        if (!nm::jsv::truthy(def) && nm::jsv::truthy(ns)) {
            def = lookup(nm::Value(nm::jsv::to_string(ns) + u"." + nm::jsv::to_string(name)));
        }
        if (!nm::jsv::truthy(def)) def = lookup(name);
        const nm::Value globals = nm::jsv::member(def, u"globals");
        if (!nm::jsv::truthy(def) || !nm::jsv::truthy(globals)) continue;
        const nm::JsString stepKey = u"step_" + nm::jsv::to_string(nm::jsv::member(info, u"stepIndex"));
        const nm::Value args = nm::jsv::member(info, u"args");
        const nm::Value rawKwargs = nm::jsv::member(info, u"rawKwargs");
        // groupGlobalsByCategory skips the hidden controls; each value is set
        // on its own, so the category order does not change the result.
        for (const auto& [key, spec] : nm::jsv::entries(globals)) {
            const nm::Value ui = nm::jsv::member(spec, u"ui");
            const nm::Value control = nm::jsv::member(ui, u"control");
            if ((control.is_bool() && !control.as_bool()) || nm::jsv::strict_equals(nm::jsv::member(ui, u"hidden"), nm::Value(true))) {
                continue;
            }
            nm::Value value = state.get_value(stepKey, key);
            if (value.is_undefined()) value = nm::jsv::member(args, key);
            if (value.is_undefined()) value = nm::jsv::member(spec, u"default");
            const nm::Value rawKwarg = nm::jsv::member(rawKwargs, key);
            const nm::Value automationValue = object_value(value) ? value : nm::jsv::member(args, key);
            const bool automated = (object_value(automationValue) &&
                                    (nm::jsv::truthy(nm::jsv::member(automationValue, u"_varRef")) || automation_type(automationValue))) ||
                                   (object_value(rawKwarg) && automation_kind(nm::jsv::member(rawKwarg, u"type")));
            if (automated) {
                const nm::Value rawType = nm::jsv::member(rawKwarg, u"type");
                if (object_value(rawKwarg) && rawType.is_string() && rawType.as_string() == u"Ident") {
                    nm::Object ref;
                    ref.set(u"_varRef", nm::jsv::member(rawKwarg, u"name"));
                    state.set_value(stepKey, key, nm::Value(std::move(ref)));
                }
                continue;
            }
            state.set_value(stepKey, key, value);
            // The control _createControlGroup picks; only a member control
            // writes the state again when it is built.
            const nm::Value type = nm::jsv::member(spec, u"type");
            const auto is = [](const nm::Value& v, const char16_t* text) { return v.is_string() && v.as_string() == text; };
            if (is(control, u"button") || is(control, u"checkbox") || is(type, u"boolean") || is(control, u"color") ||
                is(type, u"vec4") || is(type, u"vec2") || is(type, u"vec3") || nm::jsv::truthy(nm::jsv::member(spec, u"choices")) ||
                (nm::jsv::truthy(nm::jsv::member(spec, u"enum")) && is(type, u"int")) || !is(type, u"member")) {
                continue;
            }
            // _createMemberControl
            nm::Value enumPath = nm::jsv::member(spec, u"enum");
            if (!nm::jsv::truthy(enumPath)) enumPath = nm::jsv::member(spec, u"enumPath");
            const nm::Value fallback = nm::jsv::member(spec, u"default");
            if (!nm::jsv::truthy(enumPath) && fallback.is_string()) {
                const nm::JsString& text = fallback.as_string();
                const size_t dot = text.rfind(u'.');
                if (dot != nm::JsString::npos) enumPath = nm::Value(text.substr(0, dot));
            }
            if (!nm::jsv::truthy(enumPath)) continue;
            const nm::JsString path = nm::jsv::to_string(enumPath);
            nm::Value node = enums;
            size_t start = 0;
            while (true) {
                const size_t dot = path.find(u'.', start);
                const nm::JsString part = path.substr(start, dot == nm::JsString::npos ? nm::JsString::npos : dot - start);
                const nm::Value next = nm::jsv::truthy(node) ? nm::jsv::member(node, part) : nm::Value();
                if (!nm::jsv::truthy(next)) {
                    node = nm::Value::null();
                    break;
                }
                node = next;
                if (dot == nm::JsString::npos) break;
                start = dot + 1;
            }
            if (!nm::jsv::truthy(node)) continue;
            nm::Value initialValue;
            bool first = true;
            for (const nm::JsString& entry : nm::jsv::keys(node)) {
                const nm::Value fullPath(path + u"." + entry);
                const nm::Value enumEntry = nm::jsv::member(node, entry);
                const nm::Value numericValue = object_value(enumEntry) && nm::jsv::has_property(enumEntry, u"value")
                                                   ? nm::jsv::member(enumEntry, u"value")
                                                   : enumEntry;
                if (first) initialValue = fullPath;
                first = false;
                if (nm::jsv::strict_equals(numericValue, value) || nm::jsv::strict_equals(fullPath, value)) {
                    initialValue = fullPath;
                    break;
                }
            }
            state.set_value(stepKey, key, initialValue);
        }
    }
}

// Part of what the demo page does after compiling a program
// (rebuildPipelineFromDsl): the UI loads it into ProgramState (fromDsl),
// builds its controls (initialize_demo_controls) and the renderer applies
// the state's step values to the pipeline (applyStepParameterValues). The
// enum tree is a fresh page's, with the program's effects' choices
// (renderer_enum_tree).
// Returns the media textures it uploaded, which the fresh-state reset keeps.
std::vector<nm::JsString> apply_demo_step_values(be::Pipeline& pipeline, be::WebGPUBackend& backend, const Fixture& fixture,
                                                 const std::string& dslText) {
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
    nm::ProgramState<> state(registry());
    state.from_dsl(nm::utf8_to_js(dslText));
    initialize_demo_controls(state, enums);
    // The global uniforms when the page's resize draws the asyncInit
    // overlays (initAsyncEffects), before any step values are applied.
    const nm::Value initialGlobals = pipeline.globalUniforms();
    const auto apply = [&]() {
        pipeline.applyStepParameterValues(
            nm::Value(state.get_all_step_values()), [](const nm::JsString& key) { return registry().getEffectValue(key); },
            [&enums](const nm::Value& value, const nm::Value& spec) { return nm::convert_parameter_for_uniform(value, spec, enums); });
    };
    apply();
    // Media steps with passes (an externalTexture other than textTex): the page loads
    // its default image, img/testcard.png, and once it decodes
    // (_updateMediaTexture) uploads it with flipY false, sets the step's
    // imageSize to its size and applies the step values again
    // (_applyStepParams). A host texture given for the step's texture
    // replaces the image, as a loaded file does.
    std::vector<nm::JsString> media;
    std::vector<double> seenSteps;
    if (passes && passes->is_array()) {
        for (const nm::Value& pass : passes->as_array()) {
            const nm::Value key = pass.is_object() ? nm::jsv::member(pass, u"effectKey") : nm::Value();
            const nm::Value step = pass.is_object() ? nm::jsv::member(pass, u"stepIndex") : nm::Value();
            if (!key.is_string() || !step.is_number()) continue;
            if (std::find(seenSteps.begin(), seenSteps.end(), step.as_number()) != seenSteps.end()) continue;
            const nm::Value def = registry().getEffectValue(key.as_string());
            const nm::Value external = def.is_object() ? nm::jsv::member(def, u"externalTexture") : nm::Value();
            if (!nm::jsv::truthy(external) || nm::jsv::strict_equals(external, nm::Value(u"textTex"))) continue;
            seenSteps.push_back(step.as_number());
            const nm::JsString stepKey = u"step_" + nm::jsv::to_string(step);
            const nm::JsString texId = nm::jsv::to_string(external) + u"_" + stepKey;
            std::string path;
            for (const auto& [id, png] : fixture.hostTextures) {
                if (nm::utf8_to_js(id) == texId) path = png;
            }
            if (path.empty()) {
                const std::string root = environment("NM_REFERENCE_ROOT");
                if (root.empty()) throw std::runtime_error("the default media image needs NM_REFERENCE_ROOT");
                path = root + "/demo/shaders/img/testcard.png";
            }
            const nm_render::Image image = nm_render::decode_png(nm_render::read_file(path));
            const nm::Value size = backend.updateTextureFromPixels(texId, image.width, image.height, image.rgba, false);
            const double width = nm::jsv::to_number(nm::jsv::member(size, u"width"));
            const double height = nm::jsv::to_number(nm::jsv::member(size, u"height"));
            if (width > 0 && height > 0) state.set_value(stepKey, u"imageSize", nm::Value(nm::Array{nm::Value(width), nm::Value(height)}));
            media.push_back(texId);
        }
    }
    if (!media.empty()) apply();
    // The asyncInit overlays. The page's resize draws each node's overlay
    // (initAsyncEffects, _startAsyncInit) from the global uniforms, with
    // automated values at their defaults, and records the scalar values it
    // drew from. Applying the step values then calls checkAsyncRegen, which
    // draws again from the step's values when one of its scalar parameters
    // other than alpha differs from that record (automated values are
    // skipped). The last draw's canvas is uploaded as <nodeId>_overlayTex
    // with flipY true, at the pipeline's size.
    const nm::Value stepValues(state.get_all_step_values());
    std::vector<nm::JsString> seenNodes;
    if (passes && passes->is_array()) {
        for (const nm::Value& pass : passes->as_array()) {
            if (!pass.is_object()) continue;
            const nm::Value key = nm::jsv::member(pass, u"effectKey");
            const nm::Value node = nm::jsv::member(pass, u"nodeId");
            if (!key.is_string() || key.as_string().empty() || !node.is_string() || node.as_string().empty()) continue;
            if (std::find(seenNodes.begin(), seenNodes.end(), node.as_string()) != seenNodes.end()) continue;
            const std::optional<nm::AsyncOverlay> overlay = nm::asyncOverlayForEffect(key.as_string());
            if (!overlay) continue;
            seenNodes.push_back(node.as_string());
            const nm::JsString texId = node.as_string() + u"_overlayTex";
            // A host texture for the overlay is uploaded in its place below.
            if (std::any_of(fixture.hostTextures.begin(), fixture.hostTextures.end(),
                            [&](const auto& host) { return nm::utf8_to_js(host.first) == texId; })) {
                media.push_back(texId);
                continue;
            }
            const nm::Value globals = nm::jsv::member(registry().getEffectValue(key.as_string()), u"globals");
            const std::vector<nm::JsString> names = globals.is_object() ? globals.as_object().keys() : std::vector<nm::JsString>();
            // Neither undefined, null nor typeof "object".
            const auto scalar = [](const nm::Value& value) {
                return value.is_number() || value.is_string() || value.is_bool() || value.is_function();
            };
            // _startAsyncInit's drawParams[name], an automated value at its default.
            const auto drawn = [&](const nm::Value& params, const nm::JsString& name) {
                const nm::Value value = nm::jsv::member(params, name);
                return be::isAutomationValue(value) ? nm::jsv::member(nm::jsv::member(globals, name), u"default") : value;
            };
            // The scalar values the first draw records (drawParams[name] ?? drawParams[spec.uniform]).
            std::vector<std::pair<nm::JsString, nm::Value>> record;
            for (const nm::JsString& name : names) {
                nm::Value value = drawn(initialGlobals, name);
                if (value.is_undefined() || value.is_null()) {
                    const nm::Value uniform = nm::jsv::member(nm::jsv::member(globals, name), u"uniform");
                    if (uniform.is_string()) value = drawn(initialGlobals, uniform.as_string());
                }
                if (scalar(value)) record.emplace_back(name, value);
            }
            // checkAsyncRegen with the step's values.
            const nm::Value values = nm::jsv::member(stepValues, u"step_" + nm::jsv::to_string(nm::jsv::member(pass, u"stepIndex")));
            bool changed = false;
            if (values.is_object()) {
                for (const nm::JsString& name : values.as_object().keys()) {
                    if (name == u"alpha" || (!name.empty() && name[0] == u'_')) continue;
                    const nm::Value value = nm::jsv::member(values, name);
                    if (!scalar(value) || std::find(names.begin(), names.end(), name) == names.end()) continue;
                    const auto cached = std::find_if(record.begin(), record.end(), [&](const auto& entry) { return entry.first == name; });
                    if (cached == record.end() || !nm::jsv::strict_equals(cached->second, value)) changed = true;
                }
            }
            const nm::Value& params = changed ? values : initialGlobals;
            const nm::Value seed = drawn(params, u"seed");
            const nm::Value density = drawn(params, u"density");
            nm::StrokeCanvas canvas(int(pipeline.width()), int(pipeline.height()));
            const double seedNumber = nm::jsv::truthy(seed) ? nm::jsv::to_number(seed) : 1.0;
            const std::optional<double> densityNumber =
                density.is_undefined() ? std::nullopt : std::optional<double>(nm::jsv::to_number(density));
            // A trace the reference rejects (TraceResult::Failed) leaves the
            // layers drawn before it, which the reference uploaded last. An
            // allocation failure here, which the reference does not share,
            // fails the fixture.
            if (nm::drawAsyncOverlay(*overlay, canvas, seedNumber, densityNumber) == nm::TraceResult::Failed) {
                std::cerr << "nm-render: asyncInit error for " << nm::js_to_utf8(node.as_string()) << ": the trace failed\n";
            }
            backend.updateTextureFromPixels(texId, canvas.width(), canvas.height(), canvas.unpremultipliedRgba8(), true);
            media.push_back(texId);
        }
    }
    return media;
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
            // The minter registers a fixture's Portable effect before its DSL loads.
            if (const std::string sidecar = portable_sidecar(fixture); !sidecar.empty()) {
                register_portable_effect(load_portable_definition(sidecar));
            }
            const nm::Value graph = load_graph(fixture, dslText);
            be::Pipeline pipeline(graph, backend, {});
            pipeline.init(fixture.width, fixture.height);
            std::vector<nm::JsString> keep;
            if (!fixture.dsl.empty()) keep = apply_demo_step_values(pipeline, backend, fixture, dslText);
            load_meshes(graph, fixture, backend);
            deliver_midi(pipeline, fixture);
            // The minter's captures: texture contents, uploaded with flipY false.
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
