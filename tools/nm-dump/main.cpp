// Batch helpers for the differential parity gates.
#include "core/js/js_syntax.h"
#include "host_commands.h"
#include "frontend_commands.h"
#include "dsl_commands.h"
#include "program_state_commands.h"

#include <cstdio>
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

bool decode(std::string_view line, nm::JsString& result) {
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
    nm::JsString body;
    while (std::getline(std::cin, raw)) {
        if (raw.empty()) continue;
        if (!decode(raw, body)) {
            std::fputs("nm-dump js-syntax: malformed %XXXX input\n", stderr);
            return 2;
        }
        const auto result = nm::js::checkFunctionBody(u"state", body);
        std::putchar(result.verdict == nm::js::Verdict::Valid ? 'V'
                     : result.verdict == nm::js::Verdict::Invalid ? 'I' : 'U');
        std::putchar('\n');
    }
    return 0;
}

int dispatch(const std::string& subcommand, int argc, char** argv) {
    if (subcommand == "js-syntax") return cmd_js_syntax(argc, argv);
    if (subcommand == "midi-state") return nm_dump::midi_state(argc, argv);
    if (subcommand == "audio-state") return nm_dump::audio_state(argc, argv);
    if (subcommand == "obj-parser") return nm_dump::obj_parser(argc, argv);
    if (subcommand == "dump") return nm_dump::dump(argc, argv);
    if (subcommand == "registry") return nm_dump::registry(argc, argv);
    if (subcommand == "cases") return nm_dump::cases(argc, argv);
    if (subcommand == "run") return nm_dump::run_program_state(argc, argv);
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
