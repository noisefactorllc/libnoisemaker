// Tests for libnoisemaker::drawAsyncOverlay (host/async_overlays.h), and
// through it the worm tracer and stroke canvas. Plain assert-style checks.
//
// Each expected value is the 64-bit FNV-1a hash of an overlay texture the
// reference drew in Chromium 153 (the parity minter's capture
// <case>.node_1_overlayTex.png, rgba8 in texture row order, from the
// reference page at 256 x 256): the canvas's getImageData bytes with the rows
// reversed, as updateTextureFromSource(flipY true) writes them.

#include "host/async_overlays.h"
#include "host/stroke_canvas.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace {

namespace nm = libnoisemaker;

int g_failures = 0;

void check(bool condition, const char* description) {
    if (condition) {
        std::printf("PASS: %s\n", description);
    } else {
        std::printf("FAIL: %s\n", description);
        ++g_failures;
    }
}

// FNV-1a over the canvas's upload bytes, last row first.
std::uint64_t uploadedHash(const nm::StrokeCanvas& canvas) {
    const std::vector<std::uint8_t> rgba = canvas.unpremultipliedRgba8();
    const std::size_t rowBytes = std::size_t(canvas.width()) * 4;
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (int row = canvas.height() - 1; row >= 0; --row) {
        const std::uint8_t* bytes = rgba.data() + std::size_t(row) * rowBytes;
        for (std::size_t i = 0; i < rowBytes; ++i) {
            hash ^= bytes[i];
            hash *= 0x100000001b3ull;
        }
    }
    return hash;
}

void checkOverlay(nm::AsyncOverlay overlay, double seed, std::optional<double> density, std::uint64_t expected,
                  const char* description) {
    nm::StrokeCanvas canvas(256, 256);
    const nm::TraceResult result = nm::drawAsyncOverlay(overlay, canvas, seed, density);
    const std::uint64_t hash = uploadedHash(canvas);
    if (hash != expected) std::printf("  hash 0x%016llx, expected 0x%016llx\n", (unsigned long long)hash, (unsigned long long)expected);
    check(result == nm::TraceResult::Completed && hash == expected, description);
}

} // namespace

int main() {
    check(nm::asyncOverlayForEffect(u"filter.fibers") == nm::AsyncOverlay::Fibers, "filter.fibers draws fibers");
    check(nm::asyncOverlayForEffect(u"filter.scratches") == nm::AsyncOverlay::Scratches, "filter.scratches draws scratches");
    check(nm::asyncOverlayForEffect(u"filter.strayHair") == nm::AsyncOverlay::StrayHair, "filter.strayHair draws strayHair");
    check(!nm::asyncOverlayForEffect(u"filter.grain") && !nm::asyncOverlayForEffect(u"fibers"),
          "other effect keys have no overlay");

    // fibers(density: 1): filter_fibers, fibers, babylonjs_fibers.
    checkOverlay(nm::AsyncOverlay::Fibers, 1, 1.0, 0xc61a4ab2c20c437eull, "fibers, density 1, matches Chromium");
    // fibers(): threejs_fibers, touchdesigner_fibers (density undefined: 0.5).
    checkOverlay(nm::AsyncOverlay::Fibers, 1, std::nullopt, 0xd827275dae75c52full, "fibers, default density, matches Chromium");
    // scratches(): obedient and unruly layers (layer seeds 1000, 1251, 1502, 1753).
    checkOverlay(nm::AsyncOverlay::Scratches, 1, std::nullopt, 0x5a7ed006fb75caa0ull, "scratches, defaults, match Chromium");
    // strayHair(density: 1) and strayHair() draw the same overlay at this size.
    checkOverlay(nm::AsyncOverlay::StrayHair, 1, 1.0, 0xe818d5aec6fc3b06ull, "strayHair, density 1, matches Chromium");
    checkOverlay(nm::AsyncOverlay::StrayHair, 1, std::nullopt, 0xe818d5aec6fc3b06ull, "strayHair, default density, matches Chromium");

    // A canvas drawn before is cleared first, as asyncInit's new canvas is.
    {
        nm::StrokeCanvas canvas(256, 256);
        nm::drawAsyncOverlay(nm::AsyncOverlay::Fibers, canvas, 1, 1.0);
        nm::drawAsyncOverlay(nm::AsyncOverlay::Scratches, canvas, 1, std::nullopt);
        check(uploadedHash(canvas) == 0x5a7ed006fb75caa0ull, "a second draw starts from a cleared canvas");
    }

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
