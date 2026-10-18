// png_layout CANDIDATE.png PRESENTED.png -- the orientation check of
// nm_render_orientation.cmake. Exit 0 when the candidate has the presented
// image's layout: at most 1% of its pixels differ from the presented image
// by more than 64 levels in a channel, while more than 25% of them differ
// that much from the presented image flipped vertically (so the image is
// asymmetric enough to tell the two orientations apart). Exit 1 otherwise,
// 2 on unreadable input.

#include "png.h"

#include <cstdio>
#include <cstdlib>
#include <exception>

namespace {

// The share of pixels where a channel differs by more than 64 levels; with
// `flip`, against `b` upside down.
double far_share(const nm_render::Image& a, const nm_render::Image& b, bool flip) {
    size_t far = 0;
    for (uint32_t y = 0; y < a.height; ++y) {
        const uint32_t by = flip ? b.height - 1 - y : y;
        for (uint32_t x = 0; x < a.width; ++x) {
            const uint8_t* p = &a.rgba[(size_t(y) * a.width + x) * 4];
            const uint8_t* q = &b.rgba[(size_t(by) * b.width + x) * 4];
            bool differs = false;
            for (int c = 0; c < 4; ++c) differs = differs || std::abs(int(p[c]) - int(q[c])) > 64;
            far += differs;
        }
    }
    return double(far) / (double(a.width) * a.height);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: png_layout CANDIDATE.png PRESENTED.png\n");
        return 2;
    }
    nm_render::Image candidate, presented;
    try {
        candidate = nm_render::decode_png(nm_render::read_file(argv[1]));
        presented = nm_render::decode_png(nm_render::read_file(argv[2]));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "png_layout: %s\n", e.what());
        return 2;
    }
    if (candidate.width != presented.width || candidate.height != presented.height || candidate.width == 0) {
        std::fprintf(stderr, "png_layout: the candidate is %ux%u, the presented image %ux%u\n", candidate.width, candidate.height,
                     presented.width, presented.height);
        return 1;
    }
    const double upright = far_share(candidate, presented, false);
    const double flipped = far_share(candidate, presented, true);
    std::printf("png_layout: %.4f of pixels far from the presented image, %.4f from it flipped\n", upright, flipped);
    return upright <= 0.01 && flipped > 0.25 ? 0 : 1;
}
