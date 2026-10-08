#include "audio_state.h"


#include <algorithm>
#include <limits>
#include <string>
#include <cmath>

namespace nm {

namespace {

// JavaScript Math.min / Math.max: NaN if either operand is NaN.
double jsMin(double a, double b) { return (std::isnan(a) || std::isnan(b)) ? std::numeric_limits<double>::quiet_NaN() : std::min(a, b); }
double jsMax(double a, double b) { return (std::isnan(a) || std::isnan(b)) ? std::numeric_limits<double>::quiet_NaN() : std::max(a, b); }
double clamp01(double v) { return jsMax(0.0, jsMin(1.0, v)); }

// reference _avgBins: mean of bins [from, to), normalized to 0-1.
double avgBins(const std::uint8_t* buf, int length, int from, int to) {
    const int end = std::min(to, length);
    if (end <= from) return 0.0;
    double sum = 0.0;
    for (int i = from; i < end; ++i) sum += buf[i];
    return sum / (end - from) / 255.0;
}

template <typename Map, typename Key>
auto findKey(Map& map, const Key& key) {
    return std::find_if(map.begin(), map.end(), [&](const auto& entry) { return entry.first == key; });
}

template <typename Container>
HostArray numberArray(const Container& values) {
    HostArray out;
    for (const auto value : values) out.append(static_cast<double>(value));
    return out;
}

Value nameIndexValue(const JsString& id) {
    return id.empty() ? Value::null() : Value(id);
}

} // namespace

// ------------------------------------------------------------------ AudioState

AudioState::AudioState(bool deviceRegistry) : m_registry(deviceRegistry) {
    waveform.fill(0.5f);
}

AudioState::~AudioState() = default;

void AudioState::setDeviceInventory(const std::vector<AudioDevice>& devices) {
    std::vector<std::pair<JsString, JsString>> names;
    for (const AudioDevice& device : devices) {
        if (!device.connected || device.id.empty() || device.name.empty()) continue;
        auto it = findKey(names, device.name);
        if (it == names.end()) names.emplace_back(device.name, device.id);
        else if (it->second != device.id) it->second = JsString();
    }
    m_deviceInventory = std::move(names);
}

bool AudioState::registerDefaultChannels(int channelCount) {
    if (!m_registry || channelCount < 1 || channelCount > 32) return false;
    m_defaultConnected = true;
    for (int channel = 1; channel <= channelCount; ++channel) {
        if (findKey(m_defaultChannels, channel) == m_defaultChannels.end()) {
            m_defaultChannels.emplace_back(channel, std::make_unique<AudioState>(false));
        }
    }
    for (auto it = m_defaultChannels.begin(); it != m_defaultChannels.end();) {
        if (it->first > channelCount) {
            it->second->reset();
            it = m_defaultChannels.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

AudioState* AudioState::defaultChannelState(int channel) {
    if (!m_defaultConnected || channel < 1 || channel > 32) return nullptr;
    auto it = findKey(m_defaultChannels, channel);
    return it == m_defaultChannels.end() ? nullptr : it->second.get();
}

void AudioState::disconnectDefaultInput() {
    m_defaultConnected = false;
    for (auto& entry : m_defaultChannels) entry.second->reset();
}

void AudioState::updateFromAnalyser(AudioAnalyzer& analyser, double smoothing) {
    std::vector<std::uint8_t> buffer(static_cast<size_t>(analyser.frequencyBinCount()));
    analyser.getByteFrequencyData(buffer.data(), analyser.frequencyBinCount());
    updateFromFrequencyData(buffer.data(), analyser.frequencyBinCount(), smoothing);
}

void AudioState::updateFromFrequencyData(const std::uint8_t* buf, int length, double smoothing) {
    if (!buf) return;
    m_maxBufferLength = jsMax(1.0, jsMin(10.0, smoothing));

    // Bands: bins 1-2 (low), 2-12 (mid), 12-47 (high), DC bin skipped.
    const double rawLow = avgBins(buf, length, 1, 2);
    const double rawMid = avgBins(buf, length, 2, 12);
    const double rawHigh = avgBins(buf, length, 12, 47);
    low = smooth(m_smoothLow, rawLow);
    mid = smooth(m_smoothMid, rawMid);
    high = smooth(m_smoothHigh, rawHigh);

    const int step = std::max(1, length / 16);
    double sum = 0.0;
    for (int i = 0; i < 16; ++i) {
        const int index = i * step;
        const double v = index < length ? buf[index] / 255.0 : std::numeric_limits<double>::quiet_NaN();
        fft[static_cast<size_t>(i)] = static_cast<float>(v);
        sum += v;
    }
    vol = sum / 16.0;
}

double AudioState::smooth(std::vector<double>& buffer, double value) {
    buffer.push_back(value);
    if (static_cast<double>(buffer.size()) > m_maxBufferLength) buffer.erase(buffer.begin());
    double sum = 0.0;
    for (double v : buffer) sum += v;
    return sum / static_cast<double>(buffer.size());
}

void AudioState::setBands(double lowValue, double midValue, double highValue) {
    low = clamp01(lowValue);
    mid = clamp01(midValue);
    high = clamp01(highValue);
    vol = (low + mid + high) / 3.0;
}

void AudioState::setRaw(double value) {
    raw = std::isfinite(value) ? jsMax(-1.0, jsMin(1.0, value)) : 0.0;
    rawReady = true;
}

void AudioState::setRawUnavailable() {
    raw = 0.0;
    rawReady = false;
}

AudioState::DeviceEntry* AudioState::findDevice(const JsString& id) {
    auto it = std::find_if(m_devices.begin(), m_devices.end(), [&](const DeviceEntry& entry) { return entry.id == id; });
    return it == m_devices.end() ? nullptr : &*it;
}

bool AudioState::registerDevice(const AudioDevice& device) {
    if (!m_registry || device.id.empty()) return false;
    const int channelCount = device.channelCount >= 1 ? device.channelCount : 1;
    DeviceEntry* entry = findDevice(device.id);
    bool topologyChanged = false;
    if (!entry) {
        DeviceEntry created;
        created.id = device.id;
        created.name = device.name;
        created.connected = true;
        created.channelCount = channelCount;
        m_devices.push_back(std::move(created));
        entry = &m_devices.back();
        topologyChanged = true;
    } else {
        topologyChanged = entry->name != device.name || !entry->connected || entry->channelCount != channelCount;
        entry->name = device.name;
        entry->connected = true;
        entry->channelCount = channelCount;
    }
    for (int channel = 1; channel <= channelCount; ++channel) {
        if (findKey(entry->channels, channel) == entry->channels.end()) {
            entry->channels.emplace_back(channel, std::make_unique<AudioState>(false));
        }
    }
    for (auto it = entry->channels.begin(); it != entry->channels.end();) {
        if (it->first > channelCount) {
            it->second->reset();
            it = entry->channels.erase(it);
        } else {
            ++it;
        }
    }
    if (topologyChanged) rebuildDeviceNameIndex();
    return true;
}

bool AudioState::setChannelValues(const JsString& id, int channel, double lowValue, double midValue,
                                  double highValue, double volValue, double rawValue) {
    AudioState* state = deviceChannelState(id, channel);
    if (!state) return false;
    if (std::isfinite(lowValue)) state->low = clamp01(lowValue);
    if (std::isfinite(midValue)) state->mid = clamp01(midValue);
    if (std::isfinite(highValue)) state->high = clamp01(highValue);
    if (std::isfinite(volValue)) state->vol = clamp01(volValue);
    if (std::isfinite(rawValue)) state->setRaw(rawValue);
    return true;
}

void AudioState::setDeviceRawUnavailable(const JsString& id) {
    DeviceEntry* entry = m_registry ? findDevice(id) : nullptr;
    if (!entry) return;
    for (auto& channel : entry->channels) channel.second->setRawUnavailable();
}

AudioState* AudioState::deviceChannelState(const JsString& id, int channel) {
    DeviceEntry* entry = m_registry ? findDevice(id) : nullptr;
    if (!entry || !entry->connected) return nullptr;
    auto it = findKey(entry->channels, channel);
    return it == entry->channels.end() ? nullptr : it->second.get();
}

void AudioState::disconnectDevice(const JsString& id) {
    DeviceEntry* entry = m_registry ? findDevice(id) : nullptr;
    if (!entry) return;
    entry->connected = false;
    for (auto& channel : entry->channels) channel.second->reset();
    rebuildDeviceNameIndex();
}

void AudioState::rebuildDeviceNameIndex() {
    if (!m_registry) return;
    m_devicesByName.clear();
    for (const DeviceEntry& entry : m_devices) {
        if (!entry.connected || entry.name.empty()) continue;
        auto it = findKey(m_devicesByName, entry.name);
        if (it != m_devicesByName.end()) it->second = JsString();
        else m_devicesByName.emplace_back(entry.name, entry.id);
    }
}

std::vector<AudioDevice> AudioState::devices() const {
    std::vector<AudioDevice> out;
    for (const DeviceEntry& entry : m_devices) out.push_back(AudioDevice{entry.id, entry.name, entry.channelCount, entry.connected});
    return out;
}

void AudioState::setSpectrum(const std::uint8_t* frequencyData, int count) {
    const int n = std::min(count, 128);
    for (int i = 0; i < n; ++i) spectrum[static_cast<size_t>(i)] = static_cast<float>(frequencyData[i] / 255.0);
}

void AudioState::setWaveform(const std::uint8_t* timeDomainData, int count) {
    const int n = std::min(count, 128);
    for (int i = 0; i < n; ++i) waveform[static_cast<size_t>(i)] = static_cast<float>(timeDomainData[i] / 255.0);
}

void AudioState::resetAggregate() {
    low = mid = high = vol = raw = 0.0;
    rawReady = false;
    fft.fill(0.0f);
    spectrum.fill(0.0f);
    waveform.fill(0.5f);
    m_smoothLow.clear();
    m_smoothMid.clear();
    m_smoothHigh.clear();
}

void AudioState::reset() {
    resetAggregate();
    for (auto& entry : m_defaultChannels) entry.second->reset();
    for (DeviceEntry& device : m_devices) {
        for (auto& channel : device.channels) channel.second->reset();
    }
}

HostObject AudioState::stateJson(bool full) const {
    HostObject out{
        {u"low", low},
        {u"mid", mid},
        {u"high", high},
        {u"vol", vol},
        {u"raw", raw},
        {u"rawReady", rawReady},
        {u"fft", numberArray(fft)},
        {u"spectrum", numberArray(spectrum)},
        {u"waveform", numberArray(waveform)},
    };
    if (full) {
        out.insert(u"smoothing", HostObject{
            {u"low", numberArray(m_smoothLow)},
            {u"mid", numberArray(m_smoothMid)},
            {u"high", numberArray(m_smoothHigh)},
        });
        out.insert(u"maxBufferLength", m_maxBufferLength);
    }
    if (!m_registry) return out;

    HostObject devicesJson;
    for (const DeviceEntry& entry : m_devices) {
        HostObject channels;
        for (const auto& [channel, state] : entry.channels) channels.insert(utf8_to_js(std::to_string(channel)), state->stateJson(full));
        HostObject device{
            {u"name", entry.name},
            {u"connected", entry.connected},
            {u"channelCount", entry.channelCount},
            {u"channels", channels},
        };
        if (full) device.insert(u"id", entry.id);
        devicesJson.insert(entry.id, device);
    }
    out.insert(u"devices", devicesJson);
    if (full || m_defaultConnected) {
        HostObject defaults;
        for (const auto& [channel, state] : m_defaultChannels) defaults.insert(utf8_to_js(std::to_string(channel)), state->stateJson(full));
        out.insert(u"defaultChannels", defaults);
    }
    if (m_deviceInventory) {
        HostObject inventory;
        for (const auto& [name, id] : *m_deviceInventory) inventory.insert(name, nameIndexValue(id));
        out.insert(u"deviceInventory", inventory);
    }
    if (full) {
        HostObject byName;
        for (const auto& [name, id] : m_devicesByName) byName.insert(name, nameIndexValue(id));
        out.insert(u"devicesByName", byName);
        out.insert(u"defaultConnected", m_defaultConnected);
    }
    return out;
}

HostObject AudioState::snapshot() const {
    return stateJson(false);
}

HostObject AudioState::dumpState() const {
    return stateJson(true);
}

// ------------------------------------------------------------------ AudioInput

AudioInput::AudioInput() : AudioInput(Options()) {}

AudioInput::AudioInput(const Options& options) : m_options(options) {}

AudioInput::~AudioInput() = default;

AudioInput::Feed* AudioInput::feedFor(const JsString& id, int channels) {
    auto it = std::find_if(m_feeds.begin(), m_feeds.end(), [&](const auto& f) { return f->id == id; });
    Feed* f = it == m_feeds.end() ? nullptr : it->get();
    if (f && f->channels == channels) return f;
    if (!f) {
        m_feeds.push_back(std::make_unique<Feed>());
        f = m_feeds.back().get();
        f->id = id;
    }
    f->channels = channels;
    f->aggregate = id.empty() ? std::make_unique<AudioAnalyzer>(m_options.analyser) : nullptr;
    f->perChannel.clear();
    for (int c = 0; c < channels; ++c) f->perChannel.push_back(std::make_unique<AudioAnalyzer>(m_options.analyser));
    f->quantumSums.assign(static_cast<size_t>(channels), 0.0);
    f->quantumFrames = 0;
    return f;
}

void AudioInput::feed(Feed& f, const float* interleaved, std::ptrdiff_t frames) {
    const int channels = f.channels;
    const bool isDefault = f.id.empty();
    // Aggregate analyser: the first stereo pair (discrete), or mono.
    if (f.aggregate) {
        const int pair = channels == 1 ? 1 : 2;
        std::vector<float> folded(static_cast<size_t>(frames * pair));
        for (std::ptrdiff_t i = 0; i < frames; ++i) {
            for (int c = 0; c < pair; ++c) folded[static_cast<size_t>(i * pair + c)] = interleaved[i * channels + c];
        }
        f.aggregate->write(folded.data(), frames, pair);
    }
    std::vector<float> mono(static_cast<size_t>(frames));
    for (int c = 0; c < channels; ++c) {
        for (std::ptrdiff_t i = 0; i < frames; ++i) mono[static_cast<size_t>(i)] = interleaved[i * channels + c];
        f.perChannel[static_cast<size_t>(c)]->write(mono.data(), frames, 1);
    }
    // Raw control signal: one mean per channel per 128-frame render quantum
    // (Noisedeck AudioWorklet), applied as each quantum completes.
    for (std::ptrdiff_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) f.quantumSums[static_cast<size_t>(c)] += interleaved[i * channels + c];
        if (++f.quantumFrames < AudioAnalyzer::kRenderQuantumFrames) continue;
        std::vector<double> means(static_cast<size_t>(channels));
        for (int c = 0; c < channels; ++c) {
            means[static_cast<size_t>(c)] = f.quantumSums[static_cast<size_t>(c)] / AudioAnalyzer::kRenderQuantumFrames;
            f.quantumSums[static_cast<size_t>(c)] = 0.0;
        }
        f.quantumFrames = 0;
        if (isDefault) {
            const int pair = channels == 1 ? 1 : 2;
            double sum = 0.0;
            for (int c = 0; c < pair; ++c) sum += means[static_cast<size_t>(c)];
            m_state.setRaw(jsMax(-1.0, jsMin(1.0, sum / pair)));
            for (int c = 0; c < channels; ++c) {
                if (AudioState* state = m_state.defaultChannelState(c + 1)) state->setRaw(means[static_cast<size_t>(c)]);
            }
        } else {
            for (int c = 0; c < channels; ++c) {
                m_state.setChannelValues(f.id, c + 1, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN(), means[static_cast<size_t>(c)]);
            }
        }
    }
}

void AudioInput::pushDefault(const float* interleaved, std::ptrdiff_t frames, int channels) {
    if (!interleaved || frames <= 0 || channels < 1) return;
    m_state.registerDefaultChannels(channels);
    feed(*feedFor(JsString(), channels), interleaved, frames);
}

void AudioInput::disconnectDefault() {
    m_state.resetAggregate();
    m_state.disconnectDefaultInput();
    m_feeds.erase(std::remove_if(m_feeds.begin(), m_feeds.end(), [](const auto& f) { return f->id.empty(); }),
                  m_feeds.end());
}

void AudioInput::pushDevice(const AudioDevice& device, const float* interleaved, std::ptrdiff_t frames) {
    if (device.id.empty() || !interleaved || frames <= 0 || device.channelCount < 1) return;
    m_state.registerDevice(device);
    feed(*feedFor(device.id, device.channelCount), interleaved, frames);
}

void AudioInput::disconnectDevice(const JsString& id) {
    m_state.disconnectDevice(id);
    m_feeds.erase(std::remove_if(m_feeds.begin(), m_feeds.end(), [&](const auto& f) { return f->id == id; }),
                  m_feeds.end());
}

void AudioInput::applySensitivity(AudioState& state) const {
    const double gain = m_options.sensitivity;
    if (gain == 1.0) return;
    state.low = std::min(1.0, state.low * gain);
    state.mid = std::min(1.0, state.mid * gain);
    state.high = std::min(1.0, state.high * gain);
    state.vol = std::min(1.0, state.vol * gain);
}

void AudioInput::update() {
    for (const auto& f : m_feeds) {
        const bool isDefault = f->id.empty();
        if (isDefault && f->aggregate) {
            AudioAnalyzer& analyser = *f->aggregate;
            m_state.updateFromAnalyser(analyser, m_options.smoothingFrames);
            applySensitivity(m_state);
            std::vector<std::uint8_t> frequency(static_cast<size_t>(analyser.frequencyBinCount()));
            analyser.getByteFrequencyData(frequency.data(), analyser.frequencyBinCount());
            m_state.setSpectrum(frequency.data(), analyser.frequencyBinCount());
            std::vector<std::uint8_t> timeDomain(static_cast<size_t>(analyser.fftSize()));
            analyser.getByteTimeDomainData(timeDomain.data(), analyser.fftSize());
            m_state.setWaveform(timeDomain.data(), analyser.fftSize());
        }
        for (int c = 0; c < f->channels; ++c) {
            AudioState* state = isDefault ? m_state.defaultChannelState(c + 1) : m_state.deviceChannelState(f->id, c + 1);
            if (!state) continue;
            state->updateFromAnalyser(*f->perChannel[static_cast<size_t>(c)], m_options.smoothingFrames);
            applySensitivity(*state);
        }
    }
}

} // namespace nm
