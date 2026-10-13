// Batch helpers for the differential parity gates.
#include "core/js/js_syntax.h"
#include "core/value/js_math.h"
#include "host_commands.h"
#include "frontend_commands.h"
#include "dsl_commands.h"
#include "program_state_commands.h"
#include "backend_commands.h"
#include "pipeline_commands.h"
#include "uniforms_commands.h"
#include "wgsl_commands.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool decode(std::string_view line, libnoisemaker::JsString& result) {
    result.clear();
    result.reserve(line.size());
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '%') {
            if (i + 4 >= line.size()) return false;
            unsigned value = 0;
            for (int j = 1; j <= 4; ++j) {
                const int digit = hex_digit(line[i + j]);
                if (digit < 0) return false;
                value = (value << 4) | static_cast<unsigned>(digit);
            }
            result.push_back(static_cast<char16_t>(value));
            i += 4;
        } else {
            const unsigned char ch = static_cast<unsigned char>(line[i]);
            if (ch < 0x21 || ch > 0x7e) return false;
            result.push_back(static_cast<char16_t>(ch));
        }
    }
    return true;
}

int cmd_js_syntax(int, char**) {
    std::string raw;
    libnoisemaker::JsString body;
    while (std::getline(std::cin, raw)) {
        if (raw.empty()) continue;
        if (!decode(raw, body)) {
            std::fputs("nm-dump js-syntax: malformed %XXXX input\n", stderr);
            return 2;
        }
        const auto result = libnoisemaker::js::checkFunctionBody(u"state", body);
        std::putchar(result.verdict == libnoisemaker::js::Verdict::Valid ? 'V'
                     : result.verdict == libnoisemaker::js::Verdict::Invalid ? 'I' : 'U');
        std::putchar('\n');
    }
    return 0;
}

// js-math: for each line of stdin, a double as 16 hex digits of its bits,
// the bits of js::sin and js::cos of it (parity/check_js_math.mjs).
int cmd_js_math(int, char**) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        const uint64_t bits = std::strtoull(line.c_str(), nullptr, 16);
        double x = 0;
        std::memcpy(&x, &bits, sizeof x);
        const double results[2] = {libnoisemaker::js::sin(x), libnoisemaker::js::cos(x)};
        uint64_t out[2] = {};
        std::memcpy(out, results, sizeof out);
        std::printf("%016llx %016llx\n", static_cast<unsigned long long>(out[0]), static_cast<unsigned long long>(out[1]));
    }
    return 0;
}

int dispatch(const std::string& subcommand, int argc, char** argv) {
    if (subcommand == "js-syntax") return cmd_js_syntax(argc, argv);
    if (subcommand == "js-math") return cmd_js_math(argc, argv);
    if (subcommand == "midi-state") return nm_dump::midi_state(argc, argv);
    if (subcommand == "audio-state") return nm_dump::audio_state(argc, argv);
    if (subcommand == "obj-parser") return nm_dump::obj_parser(argc, argv);
    if (subcommand == "dump") return nm_dump::dump(argc, argv);
    if (subcommand == "registry") return nm_dump::registry(argc, argv);
    if (subcommand == "cases") return nm_dump::cases(argc, argv);
    if (subcommand == "run") return nm_dump::run_program_state(argc, argv);
    if (subcommand == "wgsl") return nm_dump::wgsl(argc, argv);
    if (subcommand == "uniforms") return nm_dump::uniforms(argc, argv);
    if (subcommand == "backend") return nm_dump::backend(argc, argv);
    if (subcommand == "pipeline") return nm_dump::pipeline(argc, argv);
    std::fprintf(stderr, "nm-dump: unknown subcommand: %s\n", subcommand.c_str());
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    const std::string self = std::filesystem::path(argv[0]).stem().string();
    if (self.rfind("nm-dump-", 0) == 0) return dispatch(self.substr(8), argc - 1, argv + 1);
    if (argc < 2) { std::fputs("usage: nm-dump <subcommand> ...\n", stderr); return 2; }
    return dispatch(argv[1], argc - 2, argv + 2);
}
