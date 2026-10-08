#include "audio_analyzer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nm {

namespace {

constexpr int kRingCapacity = 32768; // largest AnalyserNode fftSize
constexpr double kTwoPi = 6.283185307179586476925286766559;

// In-place iterative radix-2 complex FFT (forward, unscaled).
void fft(std::vector<double>& re, std::vector<double>& im) {
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = -kTwoPi / static_cast<double>(len);
        for (size_t start = 0; start < n; start += len) {
            for (size_t k = 0; k < len / 2; ++k) {
                const double wr = std::cos(angle * static_cast<double>(k));
                const double wi = std::sin(angle * static_cast<double>(k));
                const size_t a = start + k;
                const size_t b = a + len / 2;
                const double tr = re[b] * wr - im[b] * wi;
                const double ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

// Web Audio "speakers" down-mix of one interleaved frame to mono, in the
// operation order of Chromium's AudioBus down-mix: each scaled channel is
// accumulated in turn (vector_math::Vsma). Chromium's result differs by
// platform, and parity/check_audio_analyzer.mjs measured each oracle:
//   - macOS arm64 Chromium 151 (Apple M4): a fused multiply-add per term;
//   - Linux Chromium 153, x86_64 (GitHub ubuntu runner, CI run 35970287679)
//     and arm64 (Debian 12 arm64): a separate
//     multiply and add per term.
// The split therefore follows Apple vs everything else, not the CPU
// architecture: on Linux both the x86_64 and the arm64 oracle give the
// unfused sum, so the fused branch is kept only for Apple arm64.
// Scales of 0.5 and 0.25 are exact, so only the 5.1 sqrt(0.5) terms depend
// on this. qt/CMakeLists.txt builds the library with -ffp-contract=off so
// the unfused path is never contracted into an FMA.
float accumulate(float sum, float sample, float scale) {
#if defined(__APPLE__) && defined(__aarch64__)
    return std::fma(sample, scale, sum);
#else
    const float scaled = sample * scale;
    return sum + scaled;
#endif
}

float downMix(const float* frame, int channels) {
    switch (channels) {
    case 1:
        return frame[0];
    case 2: {
        float sum = frame[0] * 0.5f;
        return accumulate(sum, frame[1], 0.5f);
    }
    case 4: {
        float sum = frame[0] * 0.25f;
        sum = accumulate(sum, frame[1], 0.25f);
        sum = accumulate(sum, frame[2], 0.25f);
        return accumulate(sum, frame[3], 0.25f);
    }
    case 6: {
        const float root = std::sqrt(0.5f);
        float sum = frame[0] * root;
        sum = accumulate(sum, frame[1], root);
        sum = sum + frame[2];
        sum = accumulate(sum, frame[4], 0.5f);
        return accumulate(sum, frame[5], 0.5f);
    }
    default:
        return frame[0]; // discrete: keep the first channel
    }
}

} // namespace

AudioAnalyzer::AudioAnalyzer() : AudioAnalyzer(Options()) {}

AudioAnalyzer::AudioAnalyzer(const Options& options) : m_ring(kRingCapacity, 0.0f) {
    setOptions(options);
}

void AudioAnalyzer::setOptions(const Options& options) {
    const int n = options.fftSize;
    if (n < 32 || n > kRingCapacity || (n & (n - 1)) != 0) {
        throw std::invalid_argument("nm::AudioAnalyzer: fftSize must be a power of two in 32..32768");
    }
    if (!(options.minDecibels < options.maxDecibels)) {
        throw std::invalid_argument("nm::AudioAnalyzer: minDecibels must be below maxDecibels");
    }
    if (!(options.smoothingTimeConstant >= 0.0 && options.smoothingTimeConstant <= 1.0)) {
        throw std::invalid_argument("nm::AudioAnalyzer: smoothingTimeConstant must be in 0..1");
    }
    if (n != m_options.fftSize || m_magnitudes.empty()) {
        m_magnitudes.assign(static_cast<size_t>(n / 2), 0.0f);
        m_analyzedQuanta = -1;
    }
    m_options = options;
}

void AudioAnalyzer::write(const float* interleaved, std::ptrdiff_t frames, int channels) {
    if (!interleaved || frames <= 0 || channels <= 0) return;
    for (std::ptrdiff_t f = 0; f < frames; ++f) {
        m_pending.push_back(downMix(interleaved + f * channels, channels));
        if (static_cast<int>(m_pending.size()) == kRenderQuantumFrames) {
            for (float sample : m_pending) {
                m_ring[static_cast<size_t>(m_writeIndex)] = sample;
                m_writeIndex = (m_writeIndex + 1) % kRingCapacity;
            }
            m_pending.clear();
            ++m_quanta;
        }
    }
}

float AudioAnalyzer::ringSample(int index) const {
    const std::ptrdiff_t position = (m_writeIndex - m_options.fftSize + index + kRingCapacity) % kRingCapacity;
    return m_ring[static_cast<size_t>(position)];
}

void AudioAnalyzer::analyze() {
    if (m_analyzedQuanta == m_quanta) return;
    m_analyzedQuanta = m_quanta;
    const int n = m_options.fftSize;
    m_real.assign(static_cast<size_t>(n), 0.0);
    m_imag.assign(static_cast<size_t>(n), 0.0);
    // Blackman window, alpha 0.16, applied to float samples (Chromium
    // RealtimeAnalyser ApplyWindow).
    const double alpha = 0.16;
    const double a0 = 0.5 * (1.0 - alpha);
    const double a1 = 0.5;
    const double a2 = 0.5 * alpha;
    for (int i = 0; i < n; ++i) {
        const double x = static_cast<double>(i) / static_cast<double>(n);
        const double window = a0 - a1 * std::cos(kTwoPi * x) + a2 * std::cos(kTwoPi * 2.0 * x);
        m_real[static_cast<size_t>(i)] = static_cast<double>(ringSample(i) * static_cast<float>(window));
    }
    fft(m_real, m_imag);
    const double magnitudeScale = 1.0 / static_cast<double>(n);
    const double k = m_options.smoothingTimeConstant;
    for (int i = 0; i < n / 2; ++i) {
        // The DC bin carries no imaginary part; Chromium clears the packed
        // Nyquist value there.
        const double re = m_real[static_cast<size_t>(i)];
        const double im = i == 0 ? 0.0 : m_imag[static_cast<size_t>(i)];
        const double magnitude = std::hypot(re, im) * magnitudeScale;
        float smoothed = static_cast<float>(k * m_magnitudes[static_cast<size_t>(i)] + (1.0 - k) * magnitude);
        if (!std::isfinite(smoothed)) smoothed = 0.0f;
        m_magnitudes[static_cast<size_t>(i)] = smoothed;
    }
}

void AudioAnalyzer::getFloatFrequencyData(float* out, int count) {
    analyze();
    const int n = std::min(count, frequencyBinCount());
    for (int i = 0; i < n; ++i) {
        out[i] = 20.0f * std::log10(m_magnitudes[static_cast<size_t>(i)]);
    }
}

void AudioAnalyzer::getByteFrequencyData(std::uint8_t* out, int count) {
    analyze();
    const int n = std::min(count, frequencyBinCount());
    const double rangeScale = 1.0 / (m_options.maxDecibels - m_options.minDecibels);
    for (int i = 0; i < n; ++i) {
        const double db = static_cast<double>(20.0f * std::log10(m_magnitudes[static_cast<size_t>(i)]));
        const double scaled = 255.0 * (db - m_options.minDecibels) * rangeScale;
        out[i] = static_cast<std::uint8_t>(std::clamp(scaled, 0.0, 255.0));
    }
}

void AudioAnalyzer::getFloatTimeDomainData(float* out, int count) const {
    const int n = std::min(count, m_options.fftSize);
    for (int i = 0; i < n; ++i) out[i] = ringSample(i);
}

void AudioAnalyzer::getByteTimeDomainData(std::uint8_t* out, int count) const {
    const int n = std::min(count, m_options.fftSize);
    for (int i = 0; i < n; ++i) {
        // Chromium adds 1 in float, then scales in double.
        const float shifted = ringSample(i) + 1.0f;
        const double scaled = 128.0 * static_cast<double>(shifted);
        out[i] = static_cast<std::uint8_t>(std::clamp(scaled, 0.0, 255.0));
    }
}

void AudioAnalyzer::reset() {
    std::fill(m_ring.begin(), m_ring.end(), 0.0f);
    m_writeIndex = 0;
    m_pending.clear();
    m_quanta = 0;
    m_analyzedQuanta = -1;
    std::fill(m_magnitudes.begin(), m_magnitudes.end(), 0.0f);
}

} // namespace nm
