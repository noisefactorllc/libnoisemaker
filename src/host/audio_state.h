#pragma once

// audio_state.h -- engine-owned audio input state. nm::AudioState ports the
// reference shaders/src/runtime/external-input.js AudioState (band
// extraction with rolling smoothing, raw control signal, spectrum and
// waveform, per-device and default-device channel states, device
// inventory). nm::AudioInput is the capture-free host feed: it runs
// nm::AudioAnalyzer instances over interleaved float samples the way the
// Noisedeck host (app/js/features/audioInput.js) wires Web Audio, and
// updates an AudioState once per rendered frame. snapshot() returns the
// object nm::Backend::setAudioState() consumes.

#include "audio_analyzer.h"

#include "host/json_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace nm {

struct AudioDevice {
    JsString id;
    JsString name;
    int channelCount = 1;
    bool connected = true; // used by setDeviceInventory() only
};

class AudioState {
public:
    // deviceRegistry = true: the aggregate state with device and default
    // channel registries (reference `new AudioState()`); false: one channel
    // state (reference `new AudioState({ deviceRegistry: false })`).
    explicit AudioState(bool deviceRegistry = true);
    ~AudioState();
    AudioState(const AudioState&) = delete;
    AudioState& operator=(const AudioState&) = delete;

    double low = 0.0;
    double mid = 0.0;
    double high = 0.0;
    double vol = 0.0;
    double raw = 0.0;
    bool rawReady = false;
    std::array<float, 16> fft{};
    std::array<float, 128> spectrum{};
    std::array<float, 128> waveform{};

    void setDeviceInventory(const std::vector<AudioDevice>& devices);
    // Registers the independently analyzed channels of the default device
    // (1..32). Returns false for an invalid count or without a registry.
    bool registerDefaultChannels(int channelCount);
    AudioState* defaultChannelState(int channel);
    void disconnectDefaultInput();

    // reference updateFromAnalyser(analyser, smoothing): reads the
    // analyser's byte frequency data, then updates low/mid/high (rolling
    // average over `smoothing` frames, clamped 1..10), fft and vol.
    void updateFromAnalyser(AudioAnalyzer& analyser, double smoothing = 5);
    // The same update from byte frequency data the host already holds.
    void updateFromFrequencyData(const std::uint8_t* data, int count, double smoothing = 5);

    void setBands(double low, double mid, double high);
    void setRaw(double value);
    void setRawUnavailable();

    // Registers or reconnects a device (reference registerDevice). Returns
    // false without a registry or with an empty id.
    bool registerDevice(const AudioDevice& device);
    // Sets any of low/mid/high/vol (clamped 0..1) and raw for one channel of
    // a connected device. Absent values are NaN. Returns false when the
    // device or channel is unknown or disconnected.
    bool setChannelValues(const JsString& id, int channel, double low = std::numeric_limits<double>::quiet_NaN(), double mid = std::numeric_limits<double>::quiet_NaN(),
                          double high = std::numeric_limits<double>::quiet_NaN(), double vol = std::numeric_limits<double>::quiet_NaN(), double raw = std::numeric_limits<double>::quiet_NaN());
    void setDeviceRawUnavailable(const JsString& id);
    AudioState* deviceChannelState(const JsString& id, int channel);
    void disconnectDevice(const JsString& id);
    std::vector<AudioDevice> devices() const;

    void setSpectrum(const std::uint8_t* frequencyData, int count);
    void setWaveform(const std::uint8_t* timeDomainData, int count);

    void resetAggregate();
    void reset();

    // The JSON nm::Backend::setAudioState() consumes: low, mid, high, vol,
    // raw, rawReady, fft, spectrum, waveform; with a registry also devices
    // {id: {name, connected, channelCount, channels {"n": state}}},
    // defaultChannels (only while the default input is connected) and
    // deviceInventory.
    HostObject snapshot() const;
    // Complete state including smoothing buffers and name indexes, for the
    // parity gate against the reference (parity/check_audio_state.mjs).
    HostObject dumpState() const;

private:
    struct DeviceEntry {
        JsString id;
        JsString name;
        bool connected = true;
        int channelCount = 1;
        std::vector<std::pair<int, std::unique_ptr<AudioState>>> channels;
    };

    double smooth(std::vector<double>& buffer, double value);
    void rebuildDeviceNameIndex();
    DeviceEntry* findDevice(const JsString& id);
    HostObject stateJson(bool full) const;

    std::vector<double> m_smoothLow;
    std::vector<double> m_smoothMid;
    std::vector<double> m_smoothHigh;
    double m_maxBufferLength = 5;
    bool m_registry = true;
    std::vector<DeviceEntry> m_devices;
    std::vector<std::pair<JsString, JsString>> m_devicesByName; // name -> id ("" = ambiguous)
    std::optional<std::vector<std::pair<JsString, JsString>>> m_deviceInventory;
    std::vector<std::pair<int, std::unique_ptr<AudioState>>> m_defaultChannels;
    bool m_defaultConnected = false;
};

// Capture-free host feed with the Noisedeck analyser wiring:
//   - the aggregate ("legacy") analyser sees the first stereo pair of the
//     default input (a mono input stays mono), down-mixed by the analyser;
//   - each default-input and device channel has its own analyser;
//   - raw values are per-render-quantum channel means (the Noisedeck
//     AudioWorklet), the aggregate raw is the mean over the stereo pair;
//   - update() runs the Noisedeck pump once: updateFromAnalyser(analyser, 3)
//     and the sensitivity gain for every state, plus spectrum and waveform
//     for the aggregate.
class AudioInput {
public:
    struct Options {
        AudioAnalyzer::Options analyser{256, 0.5, -80.0, -30.0}; // Noisedeck makeAnalyser()
        double smoothingFrames = 3;                               // Noisedeck pump
        double sensitivity = 1.0;                                 // Noisedeck audioSensitivity setting
    };

    AudioInput();
    explicit AudioInput(const Options& options);
    ~AudioInput();

    AudioState& state() { return m_state; }
    const AudioState& state() const { return m_state; }

    // Default input: registers its channels and feeds the aggregate and the
    // per-channel analysers. `interleaved` holds frames * channels floats.
    void pushDefault(const float* interleaved, std::ptrdiff_t frames, int channels);
    void disconnectDefault();

    // A selected device: registered on first use, reconnected when needed.
    void pushDevice(const AudioDevice& device, const float* interleaved, std::ptrdiff_t frames);
    void disconnectDevice(const JsString& id);

    // One pump step; call once per rendered frame before snapshot().
    void update();

    HostObject snapshot() const { return m_state.snapshot(); }

private:
    struct Feed {
        JsString id;                      // empty for the default input
        int channels = 0;
        std::unique_ptr<AudioAnalyzer> aggregate; // default input only
        std::vector<std::unique_ptr<AudioAnalyzer>> perChannel;
        std::vector<double> quantumSums;
        int quantumFrames = 0;
    };

    Feed* feedFor(const JsString& id, int channels);
    void feed(Feed& feed, const float* interleaved, std::ptrdiff_t frames);
    void applySensitivity(AudioState& state) const;

    Options m_options;
    AudioState m_state;
    std::vector<std::unique_ptr<Feed>> m_feeds;
};

} // namespace nm
