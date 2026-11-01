#include "check.h"
#include "gpu/device.h"
#include <filesystem>
#include <fstream>

namespace {
bool inject_loss = false;
int draws = 0;
void nm_test_after_draw(libnoisemaker::gpu::Device& device) {
    ++draws;
    if (!inject_loss) return;
    inject_loss = false;
    wgpuDeviceForceLoss(device.device, WGPUDeviceLostReason_Unknown,
                        WGPUStringView{"forced renderer loss", WGPU_STRLEN});
    wgpuInstanceProcessEvents(device.instance);
    std::lock_guard<std::mutex> lock(device.errors->mutex);
    NM_CHECK(device.errors->lost);
}
}

#define main nm_render_main
#include "nm_render_loss.inc"
#undef main

int invoke(std::vector<std::string> arguments) {
    if (std::getenv("NM_ALLOW_FALLBACK_ADAPTER")) arguments.push_back("--allow-fallback");
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    return nm_render_main(static_cast<int>(argv.size()), argv.data());
}

int main(int argc, char** argv) {
    NM_CHECK(argc == 4);
    const std::string mode = argv[1], source = argv[2], work = argv[3];
    std::filesystem::create_directories(work);
    const auto program = source + "/parity/coverage/synth_testPattern.dsl";
    const auto control = work + "/control.png", failed = work + "/failed.png", later = work + "/later.png";
    for (const auto& path : {control, failed, later}) std::filesystem::remove(path);
    auto render = [&](const std::string& output) {
        return invoke({"nm-render", "render", "--dsl", program, "--out", output,
                       "--size", "32", "--frames", "2"});
    };
    NM_CHECK(render(control) == 0);
    const auto expected = nm_render::decode_png(nm_render::read_file(control));
    NM_CHECK(expected.width == 32 && expected.height == 32);
    bool varied = false;
    for (size_t i = 4; i < expected.rgba.size(); i += 4)
        varied |= !std::equal(expected.rgba.begin(), expected.rgba.begin() + 4, expected.rgba.begin() + i);
    NM_CHECK(varied);
    draws = 0;
    inject_loss = true;
    if (mode == "single") {
        NM_CHECK(render(failed) == 1);
        NM_CHECK(draws == 1);
    } else {
        NM_CHECK(mode == "batch");
        const auto manifest = work + "/batch.json";
        std::ofstream stream(manifest);
        stream << "[";
        for (const auto& output : {failed, later}) {
            if (output == later) stream << ",";
            stream << "{\"dsl\":" << nm::json::stringify(nm::Value(nm::utf8_to_js(program)))
                   << ",\"out\":" << nm::json::stringify(nm::Value(nm::utf8_to_js(output)))
                   << ",\"size\":32,\"frames\":2}";
        }
        stream << "]";
        stream.close();
        NM_CHECK(invoke({"nm-render", "batch", manifest}) == 1);
        NM_CHECK(draws == 3);
        const auto recovered = nm_render::decode_png(nm_render::read_file(later));
        NM_CHECK(recovered.width == expected.width && recovered.height == expected.height);
        NM_CHECK(recovered.rgba == expected.rgba);
    }
    NM_CHECK(!inject_loss);
    NM_CHECK(!std::filesystem::exists(failed));
    std::puts("nm-render forced-loss behavior: passed");
}
