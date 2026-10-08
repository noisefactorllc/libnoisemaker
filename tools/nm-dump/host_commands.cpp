#include "host_commands.h"

#include "core/value/json.h"
#include "host/audio_state.h"
#include "host/midi_state.h"
#include "host/obj_parser.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>

namespace nm_dump {
namespace {

const nm::Value& field(const nm::Object& object, const char16_t* name) {
    static const nm::Value missing;
    const nm::Value* value = object.find(name);
    return value ? *value : missing;
}

nm::JsString string(const nm::Value& value) {
    return value.is_string() ? value.as_string() : nm::JsString();
}

int integer(const nm::Value& value) {
    return value.is_number() ? static_cast<int>(value.as_number()) : 0;
}

bool boolean(const nm::Value& value, bool fallback = false) {
    return value.is_bool() ? value.as_bool() : fallback;
}

double number(const nm::Value& value) {
    if (value.is_number()) return value.as_number();
    if (value.is_string()) {
        if (value.as_string() == u"Infinity") return std::numeric_limits<double>::infinity();
        if (value.as_string() == u"-Infinity") return -std::numeric_limits<double>::infinity();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

const nm::Object& object(const nm::Value& value) {
    static const nm::Object empty;
    return value.is_object() ? value.as_object() : empty;
}

const nm::Array& array(const nm::Value& value) {
    static const nm::Array empty;
    return value.is_array() ? value.as_array() : empty;
}

std::vector<std::uint8_t> bytes(const nm::Value& value) {
    std::vector<std::uint8_t> out;
    for (const auto& entry : array(value)) out.push_back(static_cast<std::uint8_t>(integer(entry)));
    return out;
}

nm::Value input(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open input file");
    return nm::json::parse(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
}

int emit(const nm::Array& results) {
    const std::string output = nm::json::stringify(nm::Value(results));
    std::fwrite(output.data(), 1, output.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
}

nm::MidiPort midiPort(const nm::Object& source) {
    return {string(field(source, u"id")), string(field(source, u"name")),
            boolean(field(source, u"connected"), true)};
}

nm::AudioDevice audioDevice(const nm::Object& source) {
    return {string(field(source, u"id")), string(field(source, u"name")),
            integer(field(source, u"channelCount")), boolean(field(source, u"connected"), true)};
}

// Float32 bit strings and SHA-256 digests use the same canonical NaN and
// little-endian byte representation as the reference gate.
std::uint32_t canonicalBits(float value) {
    return std::isnan(value) ? 0x7fc00000u : std::bit_cast<std::uint32_t>(value);
}

void appendHex(std::string& out, std::uint32_t bits) {
    constexpr char digits[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4) out.push_back(digits[(bits >> shift) & 15]);
}

nm::JsString hexArray(const std::vector<float>& values) {
    std::string text;
    text.reserve(values.size() * 8);
    for (float value : values) appendHex(text, canonicalBits(value));
    return nm::utf8_to_js(text);
}

constexpr std::array<std::uint32_t, 64> kSha256{
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

nm::JsString sha256(const std::vector<float>& values) {
    std::vector<std::uint8_t> data;
    data.reserve(values.size() * 4 + 72);
    for (float value : values) {
        const std::uint32_t bits = canonicalBits(value);
        for (int shift = 0; shift < 32; shift += 8) data.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
    const std::uint64_t bitLength = static_cast<std::uint64_t>(data.size()) * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) data.push_back(static_cast<std::uint8_t>(bitLength >> shift));

    std::array<std::uint32_t, 8> hash{
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for (std::size_t block = 0; block < data.size(); block += 64) {
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            const std::size_t offset = block + static_cast<std::size_t>(i) * 4;
            w[i] = (static_cast<std::uint32_t>(data[offset]) << 24) |
                   (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
                   (static_cast<std::uint32_t>(data[offset + 2]) << 8) | data[offset + 3];
        }
        for (int i = 16; i < 64; ++i) {
            const auto s0 = std::rotr(w[i-15], 7) ^ std::rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            const auto s1 = std::rotr(w[i-2], 17) ^ std::rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        auto [a,b,c,d,e,f,g,h] = hash;
        for (int i = 0; i < 64; ++i) {
            const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto t1 = h + s1 + choose + kSha256[i] + w[i];
            const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + majority;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        hash[0]+=a; hash[1]+=b; hash[2]+=c; hash[3]+=d;
        hash[4]+=e; hash[5]+=f; hash[6]+=g; hash[7]+=h;
    }
    std::string text;
    text.reserve(64);
    for (std::uint32_t word : hash) appendHex(text, word);
    return nm::utf8_to_js(text);
}

class SilenceExpectedMeshWarnings {
public:
    SilenceExpectedMeshWarnings() : previous_(std::cerr.rdbuf(sink_.rdbuf())) {}
    ~SilenceExpectedMeshWarnings() { std::cerr.rdbuf(previous_); }
private:
    std::ostringstream sink_;
    std::streambuf* previous_;
};

}  // namespace

int midi_state(int argc, char** argv) {
    if (argc != 1) { std::fputs("usage: nm-dump midi-state <scenarios.json>\n", stderr); return 2; }
    try {
        nm::Array results;
        const auto manifest = input(argv[0]);
        for (const auto& scenarioValue : array(manifest)) {
            const auto& scenario = object(scenarioValue);
            nm::MidiState state;
            nm::Array dumps;
            for (const auto& eventValue : array(field(scenario, u"events"))) {
                const auto& event = object(eventValue);
                const auto op = string(field(event, u"op"));
                if (op == u"message") {
                    const auto data = bytes(field(event, u"data"));
                    const auto& portValue = field(event, u"port");
                    if (portValue.is_object()) {
                        const auto port = midiPort(portValue.as_object());
                        state.handleMessage(data.data(), data.size(), &port, number(field(event, u"time")));
                    } else state.handleMessage(data.data(), data.size(), nullptr, number(field(event, u"time")));
                } else if (op == u"disconnect") state.disconnectPort(string(field(event, u"id")));
                else if (op == u"register") state.registerPort(midiPort(object(field(event, u"port"))));
                else if (op == u"inventory") {
                    std::vector<nm::MidiPort> ports;
                    for (const auto& entry : array(field(event, u"ports"))) ports.push_back(midiPort(object(entry)));
                    state.setPortInventory(ports);
                } else if (op == u"reset") state.reset();
                else if (op == u"dump") dumps.emplace_back(state.dumpState());
            }
            results.emplace_back(nm::HostObject{{u"name", field(scenario, u"name")}, {u"dumps", dumps}});
        }
        return emit(results);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nm-dump midi-state: %s\n", e.what());
        return 1;
    }
}

int audio_state(int argc, char** argv) {
    if (argc != 1) { std::fputs("usage: nm-dump audio-state <scenarios.json>\n", stderr); return 2; }
    try {
        nm::Array results;
        const auto manifest = input(argv[0]);
        for (const auto& scenarioValue : array(manifest)) {
            const auto& scenario = object(scenarioValue);
            nm::AudioState state;
            nm::Array dumps;
            for (const auto& eventValue : array(field(scenario, u"events"))) {
                const auto& event = object(eventValue);
                const auto op = string(field(event, u"op"));
                const auto id = string(field(event, u"id"));
                if (op == u"update" || op == u"channelUpdate") {
                    nm::AudioState* target = &state;
                    if (op == u"channelUpdate") {
                        const int channel = integer(field(event, u"channel"));
                        target = event.has(u"id") ? state.deviceChannelState(id, channel) : state.defaultChannelState(channel);
                    }
                    const auto data = bytes(field(event, u"bytes"));
                    if (target) target->updateFromFrequencyData(data.data(), static_cast<int>(data.size()),
                                                                number(field(event, u"smoothing")));
                } else if (op == u"bands") state.setBands(number(field(event, u"low")), number(field(event, u"mid")),
                                                            number(field(event, u"high")));
                else if (op == u"raw") state.setRaw(number(field(event, u"value")));
                else if (op == u"rawUnavailable") state.setRawUnavailable();
                else if (op == u"registerDevice") state.registerDevice(audioDevice(object(field(event, u"device"))));
                else if (op == u"setChannelValues") {
                    const auto& values = object(field(event, u"values"));
                    state.setChannelValues(id, integer(field(event, u"channel")),
                        number(field(values, u"low")), number(field(values, u"mid")),
                        number(field(values, u"high")), number(field(values, u"vol")), number(field(values, u"raw")));
                } else if (op == u"deviceRawUnavailable") state.setDeviceRawUnavailable(id);
                else if (op == u"disconnectDevice") state.disconnectDevice(id);
                else if (op == u"inventory") {
                    std::vector<nm::AudioDevice> devices;
                    for (const auto& entry : array(field(event, u"devices"))) devices.push_back(audioDevice(object(entry)));
                    state.setDeviceInventory(devices);
                } else if (op == u"registerDefault") state.registerDefaultChannels(integer(field(event, u"count")));
                else if (op == u"disconnectDefault") state.disconnectDefaultInput();
                else if (op == u"spectrum" || op == u"waveform") {
                    const auto data = bytes(field(event, u"bytes"));
                    if (op == u"spectrum") state.setSpectrum(data.data(), static_cast<int>(data.size()));
                    else state.setWaveform(data.data(), static_cast<int>(data.size()));
                } else if (op == u"resetAggregate") state.resetAggregate();
                else if (op == u"reset") state.reset();
                else if (op == u"dump") dumps.emplace_back(state.dumpState());
            }
            results.emplace_back(nm::HostObject{{u"name", field(scenario, u"name")}, {u"dumps", dumps}});
        }
        return emit(results);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nm-dump audio-state: %s\n", e.what());
        return 1;
    }
}

int obj_parser(int argc, char** argv) {
    if (argc != 1) { std::fputs("usage: nm-dump obj-parser <manifest.json>\n", stderr); return 2; }
    try {
        nm::Array results;
        const auto manifest = input(argv[0]);
        for (const auto& entryValue : array(manifest)) {
            const auto& entry = object(entryValue);
            const auto path = nm::js_to_utf8(string(field(entry, u"path")));
            const auto mode = string(field(entry, u"mode"));
            nm::HostObject out{{u"path", field(entry, u"path")}, {u"mode", field(entry, u"mode")}};
            try {
                nm::ObjMeshData mesh;
                if (mode == u"file") mesh = nm::loadOBJ(path);
                else {
                    std::ifstream file(path, std::ios::binary);
                    if (!file) throw std::runtime_error("cannot open corpus file");
                    const std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
                    mesh = nm::parseOBJ(nm::utf8_to_js(data));
                }
                out.insert(u"vertexCount", static_cast<double>(mesh.vertexCount));
                out.insert(u"positions", hexArray(mesh.positions));
                out.insert(u"normals", hexArray(mesh.normals));
                out.insert(u"uvs", hexArray(mesh.uvs));
                SilenceExpectedMeshWarnings silence;
                const auto large = nm::packMeshDataForTextures(mesh, 256, 256);
                out.insert(u"packed256", nm::HostObject{
                    {u"vertexCount", large.vertexCount}, {u"positionData", sha256(large.positionData)},
                    {u"normalData", sha256(large.normalData)}, {u"uvData", sha256(large.uvData)}});
                const auto small = nm::packMeshDataForTextures(mesh, 4, 4);
                out.insert(u"packed4", nm::HostObject{
                    {u"vertexCount", small.vertexCount}, {u"positionData", hexArray(small.positionData)},
                    {u"normalData", hexArray(small.normalData)}, {u"uvData", hexArray(small.uvData)}});
            } catch (const std::exception& e) { out.insert(u"error", nm::utf8_to_js(e.what())); }
            results.emplace_back(out);
        }
        return emit(results);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nm-dump obj-parser: %s\n", e.what());
        return 1;
    }
}

}  // namespace nm_dump
