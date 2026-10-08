#include "midi_state.h"


#include <algorithm>
#include <limits>
#include <string>
#include <chrono>

namespace nm {

namespace {

// reference external-input.js module state: one note-order counter shared by
// every MidiState in the process, so orders compare across ports.
double g_midiNoteOrder = 0.0;

// RP-015 controllers that "reset all controllers" (CC121) leaves untouched.
bool retainedResetController(int cc) {
    switch (cc) {
    case 0: case 32: case 7: case 10: case 70: case 71: case 72: case 73: case 74:
    case 75: case 76: case 77: case 78: case 79: case 91: case 92: case 93: case 94: case 95:
        return true;
    default:
        return false;
    }
}

double nowMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

template <typename Map, typename Key>
auto findKey(Map& map, const Key& key) {
    return std::find_if(map.begin(), map.end(), [&](const auto& entry) { return entry.first == key; });
}

template <typename Map, typename Key, typename Value>
void setKey(Map& map, const Key& key, const Value& value) {
    auto it = findKey(map, key);
    if (it != map.end()) it->second = value;
    else map.emplace_back(key, value);
}

template <typename Map, typename Key>
void eraseKey(Map& map, const Key& key) {
    auto it = findKey(map, key);
    if (it != map.end()) map.erase(it);
}

Value optionalInt(const std::optional<int>& value) {
    return value ? Value(*value) : Value::null();
}

} // namespace

// ------------------------------------------------------------------ Channel

void MidiState::Channel::noteOn(int noteKey, int noteVelocity, const HeldNote* sourceNote,
                                const Origin& origin, double now) {
    key = noteKey;
    velocity = noteVelocity;
    gate = 1;
    time = sourceNote ? sourceNote->time : now;
    keys[noteKey] = static_cast<std::uint8_t>(noteVelocity);
    HeldNote note;
    note.key = noteKey;
    note.velocity = noteVelocity;
    note.time = time;
    note.order = sourceNote ? sourceNote->order : ++g_midiNoteOrder;
    note.origin = origin;
    auto it = std::find_if(heldNotes.begin(), heldNotes.end(), [&](const HeldNote& held) { return held.key == noteKey; });
    if (it != heldNotes.end()) *it = note;
    else heldNotes.push_back(note);
}

std::optional<MidiState::ParameterChange> MidiState::Channel::controlChange(int controller, int value) {
    if (controller < 0 || controller > 127 || value < 0 || value > 127) return std::nullopt;
    cc[controller] = static_cast<std::uint8_t>(value);
    if (controller < 64) {
        const int msb = controller & 31;
        cc14[msb] = static_cast<std::uint16_t>((cc[msb] << 7) | cc[msb + 32]);
    }
    if (controller == 121) {
        resetControllers();
    } else if (controller == 120 || controller == 123) {
        clearNotes();
    } else if (controller == 99 || controller == 98 || controller == 101 || controller == 100) {
        const bool isRpn = controller >= 100;
        parameterFamily = isRpn;
        auto& selectors = isRpn ? rpnSelectors : nrpnSelectors;
        selectors[(controller == 99 || controller == 101) ? 0 : 1] = value;
    } else if ((controller == 6 || controller == 38 || controller == 96 || controller == 97) && parameterFamily) {
        const bool isRpn = *parameterFamily;
        const auto& selectors = isRpn ? rpnSelectors : nrpnSelectors;
        if (!selectors[0] || !selectors[1] || (*selectors[0] == 127 && *selectors[1] == 127)) return std::nullopt;
        const int parameter = (*selectors[0] << 7) | *selectors[1];
        auto& family = isRpn ? rpn : nrpn;
        const auto existing = findKey(family, parameter);
        const int previous = existing != family.end() ? existing->second : 0;
        int next = 0;
        if (controller == 6) next = value << 7;
        else if (controller == 38) next = (previous & 0x3f80) | value;
        else next = std::max(0, std::min(16383, previous + (controller == 96 ? 1 : -1)));
        setKey(family, parameter, next);
        ParameterChange change;
        change.rpn = isRpn;
        change.parameter = parameter;
        change.value = next;
        return change;
    }
    return std::nullopt;
}

void MidiState::Channel::resetControllers() {
    for (int c = 0; c < 128; ++c) {
        if (!retainedResetController(c)) cc[c] = (c == 11 || (c >= 98 && c <= 101)) ? 127 : 0;
    }
    for (int c = 0; c < 32; ++c) cc14[c] = static_cast<std::uint16_t>((cc[c] << 7) | cc[c + 32]);
    pitchBend = 8192;
    pressure = 0;
    polyPressure.fill(0);
    parameterFamily.reset();
    nrpnSelectors = {};
    rpnSelectors = {};
}

void MidiState::Channel::clearNotes() {
    gate = 0;
    keys.fill(0);
    heldNotes.clear();
    polyPressure.fill(0);
}

void MidiState::Channel::noteOff(int noteKey) {
    gate = 0;
    keys[noteKey] = 0;
    heldNotes.erase(std::remove_if(heldNotes.begin(), heldNotes.end(),
                                   [&](const HeldNote& held) { return held.key == noteKey; }),
                    heldNotes.end());
    polyPressure[noteKey] = 0;
}

void MidiState::Channel::reset() {
    *this = Channel();
}

// ------------------------------------------------------------------ MidiState

MidiState::MidiState(bool portRegistry) : m_registry(portRegistry) {
    if (portRegistry) m_unscoped = std::make_unique<MidiState>(false);
}

MidiState::~MidiState() = default;

MidiState::Channel& MidiState::channel(int number) {
    // reference getChannel: invalid numbers fall back to channel 1.
    if (number < 1 || number > 16) number = 1;
    return m_channels[number - 1];
}

MidiState::PortEntry* MidiState::findPort(const JsString& id) {
    auto it = std::find_if(m_ports.begin(), m_ports.end(), [&](const PortEntry& entry) { return entry.id == id; });
    return it == m_ports.end() ? nullptr : &*it;
}

const MidiState::PortEntry* MidiState::findPort(const JsString& id) const {
    auto it = std::find_if(m_ports.begin(), m_ports.end(), [&](const PortEntry& entry) { return entry.id == id; });
    return it == m_ports.end() ? nullptr : &*it;
}

MidiState* MidiState::registerPort(const MidiPort& port) {
    if (!m_registry || port.id.empty()) return nullptr;
    PortEntry* entry = findPort(port.id);
    bool topologyChanged = false;
    if (!entry) {
        PortEntry created;
        created.id = port.id;
        created.name = port.name;
        created.connected = true;
        created.state = std::make_unique<MidiState>(false);
        m_ports.push_back(std::move(created));
        entry = &m_ports.back();
        topologyChanged = true;
    } else {
        topologyChanged = entry->name != port.name || !entry->connected;
        entry->name = port.name;
        entry->connected = true;
    }
    MidiState* state = entry->state.get();
    if (topologyChanged) rebuildPortNameIndex();
    return state;
}

void MidiState::disconnectPort(const JsString& id) {
    PortEntry* entry = m_registry ? findPort(id) : nullptr;
    if (!entry) return;
    entry->connected = false;
    entry->state->reset();
    Origin origin;
    origin.kind = Origin::Port;
    origin.id = id;
    for (Channel& ch : m_channels) {
        for (int c = 0; c < 128; ++c) {
            if (ch.ccPorts[c] == origin) {
                ch.cc[c] = 0;
                ch.ccPorts[c] = Origin();
            }
        }
        for (int c = 0; c < 32; ++c) {
            if (ch.cc14Ports[c] == origin) {
                ch.cc14[c] = 0;
                ch.cc14Ports[c] = Origin();
            }
        }
    }
    for (Channel& ch : m_channels) {
        clearNoteOrigin(ch, origin);
        for (int family = 0; family < 2; ++family) {
            auto& ports = family == 0 ? ch.nrpnPorts : ch.rpnPorts;
            auto& values = family == 0 ? ch.nrpn : ch.rpn;
            const auto snapshot = ports;
            for (const auto& [parameter, from] : snapshot) {
                if (from != origin) continue;
                eraseKey(values, parameter);
                eraseKey(ports, parameter);
            }
        }
        if (ch.pitchBendPort == origin) {
            ch.pitchBend = 8192;
            ch.pitchBendPort = Origin();
        }
        if (ch.pressurePort == origin) {
            ch.pressure = 0;
            ch.pressurePort = Origin();
        }
        for (int k = 0; k < 128; ++k) {
            if (ch.polyPressurePorts[k] != origin) continue;
            ch.polyPressure[k] = 0;
            ch.polyPressurePorts[k] = Origin();
        }
    }
    rebuildPortNameIndex();
}

void MidiState::setPortInventory(const std::vector<MidiPort>& ports) {
    std::vector<std::pair<JsString, JsString>> names;
    for (const MidiPort& port : ports) {
        if (!port.connected || port.id.empty() || port.name.empty()) continue;
        auto it = findKey(names, port.name);
        if (it == names.end()) names.emplace_back(port.name, port.id);
        else if (it->second != port.id) it->second = JsString();
    }
    m_portInventory = std::move(names);
}

void MidiState::rebuildPortNameIndex() {
    if (!m_registry) return;
    m_portsByName.clear();
    for (const PortEntry& entry : m_ports) {
        if (!entry.connected || entry.name.empty()) continue;
        auto it = findKey(m_portsByName, entry.name);
        if (it != m_portsByName.end()) it->second = JsString();
        else m_portsByName.emplace_back(entry.name, entry.id);
    }
}

std::vector<MidiPort> MidiState::ports() const {
    std::vector<MidiPort> result;
    for (const PortEntry& entry : m_ports) result.push_back(MidiPort{entry.id, entry.name, entry.connected});
    return result;
}

std::vector<int> MidiState::configureMpeZone(int master, int count) {
    if ((master != 1 && master != 16) || count > 15) return {};
    const std::optional<int> previousLower = m_mpeLower;
    const std::optional<int> previousUpper = m_mpeUpper;
    std::optional<int>& zone = master == 1 ? m_mpeLower : m_mpeUpper;
    std::optional<int>& other = master == 1 ? m_mpeUpper : m_mpeLower;
    zone = count;
    if (!other) other = 0;
    if (count > 0 && count + *other > 14) other = std::max(0, 14 - count);

    // JS `null > 0` is false, so an unset zone owns no channel.
    auto owner = [](const std::optional<int>& lower, const std::optional<int>& upper, int ch) -> int {
        const int lo = lower.value_or(0);
        const int up = upper.value_or(0);
        if (lo > 0 && ch == 1) return 1;                       // lowerManager
        if (up > 0 && ch == 16) return 2;                      // upperManager
        if (lo > 0 && ch >= 2 && ch <= lo + 1) return 3;       // lower
        if (up > 0 && ch >= 16 - up && ch <= 15) return 4;     // upper
        return 0;
    };
    std::vector<int> changed;
    for (int ch = 1; ch <= 16; ++ch) {
        if (owner(previousLower, previousUpper, ch) == owner(m_mpeLower, m_mpeUpper, ch)) continue;
        changed.push_back(ch);
        Channel& state = m_channels[ch - 1];
        const auto nrpnSelectors = state.nrpnSelectors;
        const auto rpnSelectors = state.rpnSelectors;
        const auto family = state.parameterFamily;
        const std::array<std::uint8_t, 4> selectorBytes{state.cc[98], state.cc[99], state.cc[100], state.cc[101]};
        state.clearNotes();
        state.resetControllers();
        // MCM is not CC121: keep parameter selection so further Data Entry
        // remains valid.
        state.nrpnSelectors = nrpnSelectors;
        state.rpnSelectors = rpnSelectors;
        state.parameterFamily = family;
        for (int i = 0; i < 4; ++i) state.cc[98 + i] = selectorBytes[i];
        // Neutral timbre is an adapter default, not a mandated CC74 reset.
        state.cc[74] = 64;
    }
    return changed;
}

void MidiState::clearNoteOrigin(Channel& ch, const Origin& origin) {
    for (auto it = ch.heldNotes.begin(); it != ch.heldNotes.end();) {
        if (it->origin != origin) {
            ++it;
            continue;
        }
        ch.keys[it->key] = 0;
        it = ch.heldNotes.erase(it);
    }
    const bool stillHeld = std::any_of(ch.heldNotes.begin(), ch.heldNotes.end(),
                                       [&](const HeldNote& held) { return held.key == ch.key; });
    if (!stillHeld) ch.gate = 0;
}

void MidiState::copyControllerReset(Channel& ch, const Channel& source, const Origin& origin, bool resetTimbre) {
    for (int c = 0; c < 128; ++c) {
        if ((retainedResetController(c) && !(resetTimbre && c == 74))
            || (ch.ccPorts[c] != origin && ch.ccPorts[c].kind != Origin::None)) continue;
        ch.cc[c] = source.cc[c];
        ch.ccPorts[c] = origin;
    }
    for (int c = 0; c < 32; ++c) {
        if (ch.cc14Ports[c] != origin && ch.cc14Ports[c].kind != Origin::None) continue;
        ch.cc14[c] = source.cc14[c];
        ch.cc14Ports[c] = origin;
    }
    if (ch.pitchBendPort == origin || ch.pitchBendPort.kind == Origin::None) ch.pitchBend = 8192;
    if (ch.pressurePort == origin || ch.pressurePort.kind == Origin::None) ch.pressure = 0;
    for (int k = 0; k < 128; ++k) {
        if (ch.polyPressurePorts[k] == origin) ch.polyPressure[k] = 0;
    }
}

void MidiState::handleMessage(const std::uint8_t* data, std::ptrdiff_t length, const MidiPort* port) {
    handleMessageImpl(data, length, port, nowMs());
}

void MidiState::handleMessage(const std::uint8_t* data, std::ptrdiff_t length, const MidiPort* port, double timestampMs) {
    handleMessageImpl(data, length, port, timestampMs);
}

std::optional<MidiState::ParameterChange> MidiState::handleMessageImpl(const std::uint8_t* data, std::ptrdiff_t length,
                                                                       const MidiPort* port, double timestampMs) {
    if (!data || length < 1) return std::nullopt;
    MidiState* sourceState = nullptr;
    if (m_registry) sourceState = port ? registerPort(*port) : m_unscoped.get();
    if (port && m_registry && !sourceState) return std::nullopt;
    const std::optional<ParameterChange> parameterChange =
        sourceState ? sourceState->handleMessageImpl(data, length, nullptr, timestampMs) : std::nullopt;

    const int status = data[0];
    if (status == 0xf8) {
        m_clockCount += 1.0;
        return std::nullopt;
    }
    if (length < 2) return std::nullopt;
    const int key = data[1];
    const int channelNumber = (status & 0x0f) + 1;
    const int messageType = status & 0xf0;
    if (key > 127) return std::nullopt;
    const bool hasVelocity = length >= 3;
    const int velocity = hasVelocity ? data[2] : -1;
    if (messageType != 0xd0 && (!hasVelocity || velocity > 127)) return std::nullopt;

    Channel& ch = channel(channelNumber);
    const Channel* source = sourceState ? &sourceState->channel(channelNumber) : nullptr;
    Origin origin;
    if (port) {
        origin.kind = Origin::Port;
        origin.id = port->id;
    } else {
        origin.kind = Origin::Unscoped;
    }

    if (messageType == 0xe0) {
        ch.pitchBend = key | (velocity << 7);
        ch.pitchBendPort = origin;
        return std::nullopt;
    }
    if (messageType == 0xd0) {
        ch.pressure = key;
        ch.pressurePort = origin;
        return std::nullopt;
    }
    if (messageType == 0xa0) {
        ch.polyPressure[key] = static_cast<std::uint8_t>(velocity);
        ch.polyPressurePorts[key] = origin;
        return std::nullopt;
    }
    if (messageType == 0xb0) {
        if (!source) {
            std::optional<ParameterChange> change = ch.controlChange(key, velocity);
            if (change && change->rpn && change->parameter == 6 && key == 6) {
                change->resetChannels = configureMpeZone(channelNumber, velocity);
            }
            return change;
        }
        ch.cc[key] = source->cc[key];
        ch.ccPorts[key] = origin;
        if (key < 64) {
            const int msb = key & 31;
            ch.cc14[msb] = source->cc14[msb];
            ch.cc14Ports[msb] = origin;
        }
        if (parameterChange) {
            auto& values = parameterChange->rpn ? ch.rpn : ch.nrpn;
            auto& ports = parameterChange->rpn ? ch.rpnPorts : ch.nrpnPorts;
            setKey(values, parameterChange->parameter, parameterChange->value);
            setKey(ports, parameterChange->parameter, origin);
            for (int index : parameterChange->resetChannels) {
                clearNoteOrigin(m_channels[index - 1], origin);
                copyControllerReset(m_channels[index - 1], sourceState->m_channels[index - 1], origin, true);
            }
        }
        if (key == 120 || key == 123) {
            clearNoteOrigin(ch, origin);
            for (int note = 0; note < 128; ++note) {
                if (ch.polyPressurePorts[note] == origin) ch.polyPressure[note] = 0;
            }
        }
        if (key == 121) copyControllerReset(ch, *source, origin);
        return parameterChange;
    }
    if (messageType == 0x90 && velocity > 0) {
        const HeldNote* sourceNote = nullptr;
        if (source) {
            auto it = std::find_if(source->heldNotes.begin(), source->heldNotes.end(),
                                   [&](const HeldNote& held) { return held.key == key; });
            if (it != source->heldNotes.end()) sourceNote = &*it;
        }
        ch.noteOn(key, velocity, sourceNote, origin, timestampMs);
    } else if (messageType == 0x80 || (messageType == 0x90 && velocity == 0)) {
        if (!source) {
            ch.noteOff(key);
        } else {
            // Keep the aggregate gate behavior without erasing a same-key
            // held note or pressure supplied by another port.
            ch.gate = 0;
            auto it = std::find_if(ch.heldNotes.begin(), ch.heldNotes.end(),
                                   [&](const HeldNote& held) { return held.key == key; });
            if (it != ch.heldNotes.end() && it->origin == origin) {
                ch.keys[key] = 0;
                ch.heldNotes.erase(it);
            }
            if (ch.polyPressurePorts[key] == origin) ch.polyPressure[key] = 0;
        }
    }
    return std::nullopt;
}

void MidiState::reset() {
    for (Channel& ch : m_channels) ch.reset();
    m_clockCount = 0.0;
    m_mpeLower.reset();
    m_mpeUpper.reset();
    if (m_unscoped) m_unscoped->reset();
    for (PortEntry& entry : m_ports) entry.state->reset();
}

// ------------------------------------------------------------------ JSON

namespace {

template <typename Container>
HostArray numberArray(const Container& values) {
    HostArray out;
    for (const auto value : values) out.append(static_cast<double>(value));
    return out;
}

template <typename Map>
HostObject numberMap(const Map& values) {
    HostObject out;
    for (const auto& [key, value] : values) out.insert(utf8_to_js(std::to_string(key)), value);
    return out;
}

} // namespace

HostObject MidiState::channelJson(const Channel& ch, bool full) const {
    auto originJson = [](const Origin& origin) -> Value {
        if (origin.kind == Origin::None) return Value::null();
        if (origin.kind == Origin::Unscoped) return u"<unscoped>";
        return u"port:" + origin.id;
    };
    HostObject out;
    out.insert(u"key", ch.key);
    out.insert(u"velocity", ch.velocity);
    out.insert(u"gate", ch.gate);
    out.insert(u"time", ch.time);
    out.insert(u"keys", numberArray(ch.keys));
    out.insert(u"cc", numberArray(ch.cc));
    out.insert(u"cc14", numberArray(ch.cc14));
    out.insert(u"pitchBend", ch.pitchBend);
    out.insert(u"pressure", ch.pressure);
    out.insert(u"polyPressure", numberArray(ch.polyPressure));
    out.insert(u"nrpn", numberMap(ch.nrpn));
    out.insert(u"rpn", numberMap(ch.rpn));
    HostObject held;
    for (const HeldNote& note : ch.heldNotes) {
        HostObject entry{
            {u"key", note.key},
            {u"velocity", note.velocity},
            {u"time", note.time},
            {u"order", note.order},
        };
        if (full) entry.insert(u"origin", originJson(note.origin));
        held.insert(utf8_to_js(std::to_string(note.key)), entry);
    }
    out.insert(u"heldNotes", held);
    if (!full) return out;

    HostArray ccPorts;
    for (const Origin& origin : ch.ccPorts) ccPorts.append(originJson(origin));
    HostArray cc14Ports;
    for (const Origin& origin : ch.cc14Ports) cc14Ports.append(originJson(origin));
    HostArray polyPorts;
    for (const Origin& origin : ch.polyPressurePorts) polyPorts.append(originJson(origin));
    out.insert(u"ccPorts", ccPorts);
    out.insert(u"cc14Ports", cc14Ports);
    out.insert(u"polyPressurePorts", polyPorts);
    out.insert(u"pitchBendPort", originJson(ch.pitchBendPort));
    out.insert(u"pressurePort", originJson(ch.pressurePort));
    HostObject nrpnPorts;
    for (const auto& [parameter, origin] : ch.nrpnPorts) nrpnPorts.insert(utf8_to_js(std::to_string(parameter)), originJson(origin));
    HostObject rpnPorts;
    for (const auto& [parameter, origin] : ch.rpnPorts) rpnPorts.insert(utf8_to_js(std::to_string(parameter)), originJson(origin));
    out.insert(u"nrpnPorts", nrpnPorts);
    out.insert(u"rpnPorts", rpnPorts);
    out.insert(u"selectors", HostObject{
        {u"nrpn", HostArray{optionalInt(ch.nrpnSelectors[0]), optionalInt(ch.nrpnSelectors[1])}},
        {u"rpn", HostArray{optionalInt(ch.rpnSelectors[0]), optionalInt(ch.rpnSelectors[1])}},
    });
    out.insert(u"parameterFamily",
               ch.parameterFamily ? Value(*ch.parameterFamily ? u"rpn" : u"nrpn")
                                  : Value::null());
    return out;
}

HostObject MidiState::stateJson(bool full) const {
    HostObject out;
    out.insert(u"clockCount", m_clockCount);
    out.insert(u"mpeZones", HostObject{
        {u"lower", optionalInt(m_mpeLower)},
        {u"upper", optionalInt(m_mpeUpper)},
    });
    HostObject channels;
    for (int n = 1; n <= 16; ++n) channels.insert(utf8_to_js(std::to_string(n)), channelJson(m_channels[n - 1], full));
    out.insert(u"channels", channels);
    if (!m_registry) return out;

    HostObject ports;
    for (const PortEntry& entry : m_ports) {
        HostObject port{
            {u"name", entry.name},
            {u"connected", entry.connected},
            {u"state", entry.state->stateJson(full)},
        };
        if (full) port.insert(u"id", entry.id);
        ports.insert(entry.id, port);
    }
    out.insert(u"ports", ports);
    out.insert(u"unscopedState", m_unscoped->stateJson(full));
    if (m_portInventory) {
        HostObject inventory;
        for (const auto& [name, id] : *m_portInventory) {
            inventory.insert(name, id.empty() ? Value::null() : Value(id));
        }
        out.insert(u"portInventory", inventory);
    }
    if (full) {
        HostObject byName;
        for (const auto& [name, id] : m_portsByName) {
            byName.insert(name, id.empty() ? Value::null() : Value(id));
        }
        out.insert(u"portsByName", byName);
    }
    return out;
}

HostObject MidiState::snapshot() const {
    return stateJson(false);
}

HostObject MidiState::dumpState() const {
    return stateJson(true);
}

} // namespace nm
