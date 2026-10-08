#pragma once

// audio_analyzer.h -- a Web Audio AnalyserNode equivalent for hosts that
// capture audio themselves. Follows the Web Audio specification and the
// Chromium implementation the reference hosts run on: speaker down-mix to
// mono, a ring of recent samples advanced in 128-frame render quanta,
// Blackman window (alpha 0.16), FFT of fftSize, magnitude / fftSize,
// smoothingTimeConstant averaging, dB conversion and byte scaling between
// minDecibels and maxDecibels. Frequency data is computed at most once per
// render quantum, as AnalyserNode does; a second read in the same quantum
// returns the same data without smoothing again.
//
// The analyser is independent of the sample rate: bin k covers
// k * sampleRate / fftSize Hz, which the host knows. No capture code.

#include <cstddef>
#include <cstdint>

#include <vector>

namespace nm {

class AudioAnalyzer {
public:
    static constexpr int kRenderQuantumFrames = 128;

    struct Options {
        int fftSize = 2048;                  // power of two, 32..32768 (AnalyserNode default 2048)
        double smoothingTimeConstant = 0.8;  // 0..1
        double minDecibels = -100.0;
        double maxDecibels = -30.0;
    };

    AudioAnalyzer();
    explicit AudioAnalyzer(const Options& options);

    // Throws std::invalid_argument for an fftSize that AnalyserNode rejects,
    // or minDecibels >= maxDecibels, or a smoothing constant outside 0..1.
    void setOptions(const Options& options);
    const Options& options() const { return m_options; }
    int fftSize() const { return m_options.fftSize; }
    int frequencyBinCount() const { return m_options.fftSize / 2; }

    // Appends interleaved float samples. The analyser down-mixes them to
    // mono with the Web Audio "speakers" rules (1, 2, 4 and 6 channels;
    // other counts keep channel 1). Frames join the analysis ring only in
    // complete 128-frame render quanta; a partial quantum waits for the
    // next write.
    void write(const float* interleaved, std::ptrdiff_t frames, int channels);

    // Number of complete render quanta written so far.
    std::int64_t renderQuanta() const { return m_quanta; }

    // AnalyserNode getters. `out` receives min(count, required) values:
    // frequencyBinCount for frequency data, fftSize for time-domain data.
    void getFloatFrequencyData(float* out, int count);
    void getByteFrequencyData(std::uint8_t* out, int count);
    void getFloatTimeDomainData(float* out, int count) const;
    void getByteTimeDomainData(std::uint8_t* out, int count) const;

    // Clears the sample ring, the pending partial quantum, and the smoothed
    // magnitudes.
    void reset();

private:
    void analyze();
    float ringSample(int index) const; // index 0 = oldest of the last fftSize samples

    Options m_options;
    std::vector<float> m_ring;        // capacity = 32768 (the largest fftSize)
    std::ptrdiff_t m_writeIndex = 0;
    std::vector<float> m_pending;     // mono frames of an incomplete render quantum
    std::int64_t m_quanta = 0;
    std::int64_t m_analyzedQuanta = -1;
    std::vector<float> m_magnitudes;  // smoothed |X[k]| / fftSize
    std::vector<double> m_windowed;
    std::vector<double> m_real;
    std::vector<double> m_imag;
};

} // namespace nm
