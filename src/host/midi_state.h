#pragma once

// midi_state.h -- engine-owned MIDI input state. Port of the reference
// shaders/src/runtime/external-input.js MidiChannelState / MidiState
// (message parsing, 14-bit CC pairing, RPN/NRPN, MPE zone configuration,
// per-port isolated state with an aggregate view, and the unscoped state).
//
// The host feeds raw MIDI message bytes with a stable port identity; it
// never builds state JSON by hand. snapshot() returns the object that
// nm::Backend::setMidiState() consumes. The library contains no MIDI
// capture or device code.

#include "host/json_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace nm {

// Identity of one MIDI input port (reference Web MIDI {id, name}).
struct MidiPort {
    JsString id;
    JsString name;
    bool connected = true; // used by setPortInventory() only
};

class MidiState {
public:
    // portRegistry = true: the aggregate state with per-port isolated
    // states and an unscoped state (reference `new MidiState()`).
    // portRegistry = false: one isolated state (reference
    // `new MidiState({ portRegistry: false })`).
    explicit MidiState(bool portRegistry = true);
    ~MidiState();
    MidiState(const MidiState&) = delete;
    MidiState& operator=(const MidiState&) = delete;

    // Processes one raw MIDI message (reference handleMessage). `port` null
    // means a message without a port (the unscoped source). `timestampMs`
    // is the note-on time in epoch milliseconds (the reference uses
    // Date.now()); the overload without it uses the system clock.
    void handleMessage(const std::uint8_t* data, std::ptrdiff_t length, const MidiPort* port = nullptr);
    void handleMessage(const std::uint8_t* data, std::ptrdiff_t length, const MidiPort* port, double timestampMs);

    // Registers or reconnects a port (reference registerPort). Returns its
    // isolated state, or nullptr without a registry or with an empty id.
    MidiState* registerPort(const MidiPort& port);
    // Marks a port disconnected, resets its isolated state, and clears the
    // aggregate values that came from it (reference disconnectPort).
    void disconnectPort(const JsString& id);
    // Physical port discovery, independent of which ports are open
    // (reference setPortInventory). Name selectors then resolve through it.
    void setPortInventory(const std::vector<MidiPort>& ports);
    // Registered ports with their connection state (reference getPorts).
    std::vector<MidiPort> ports() const;

    // Resets every channel, the clock, MPE zones and all port states.
    void reset();

    double clockCount() const { return m_clockCount; }

    // The JSON nm::Backend::setMidiState() consumes: clockCount, mpeZones,
    // channels "1".."16" (key, velocity, gate, time, keys, cc, cc14, nrpn,
    // rpn, pitchBend, pressure, polyPressure, heldNotes), and with a
    // registry also ports {id: {name, connected, state}}, unscopedState
    // and portInventory.
    HostObject snapshot() const;

    // Complete state including the per-origin bookkeeping, for the parity
    // gate against the reference (parity/check_midi_state.mjs).
    HostObject dumpState() const;

private:
    // Where a stored value came from: nothing, the unscoped source, or a port id.
    struct Origin {
        enum Kind { None, Unscoped, Port } kind = None;
        JsString id;
        bool operator==(const Origin& other) const { return kind == other.kind && id == other.id; }
        bool operator!=(const Origin& other) const { return !(*this == other); }
    };
    struct HeldNote {
        int key = 0;
        int velocity = 0;
        double time = 0.0;
        double order = 0.0;
        Origin origin;
    };
    struct ParameterChange {
        bool rpn = false; // family: false = nrpn, true = rpn
        int parameter = 0;
        int value = 0;
        std::vector<int> resetChannels;
    };
    struct Channel {
        int key = 0;
        int velocity = 0;
        int gate = 0;
        double time = 0.0;
        std::array<std::uint8_t, 128> keys{};
        std::array<std::uint8_t, 128> cc{};
        std::array<std::uint16_t, 32> cc14{};
        std::array<Origin, 128> ccPorts{};
        std::array<Origin, 32> cc14Ports{};
        int pitchBend = 8192;
        int pressure = 0;
        std::array<std::uint8_t, 128> polyPressure{};
        std::vector<std::pair<int, int>> nrpn; // insertion-ordered Map
        std::vector<std::pair<int, int>> rpn;
        std::vector<HeldNote> heldNotes;       // insertion-ordered Map keyed by note.key
        std::array<std::optional<int>, 2> nrpnSelectors{};
        std::array<std::optional<int>, 2> rpnSelectors{};
        std::optional<bool> parameterFamily;   // nullopt, false = nrpn, true = rpn
        std::vector<std::pair<int, Origin>> nrpnPorts;
        std::vector<std::pair<int, Origin>> rpnPorts;
        Origin pitchBendPort;
        Origin pressurePort;
        std::array<Origin, 128> polyPressurePorts{};

        void noteOn(int key, int velocity, const HeldNote* sourceNote, const Origin& origin, double now);
        std::optional<ParameterChange> controlChange(int controller, int value);
        void resetControllers();
        void clearNotes();
        void noteOff(int key);
        void reset();
    };
    struct PortEntry {
        JsString id;
        JsString name;
        bool connected = true;
        std::unique_ptr<MidiState> state;
    };

    std::optional<ParameterChange> handleMessageImpl(const std::uint8_t* data, std::ptrdiff_t length,
                                                     const MidiPort* port, double timestampMs);
    Channel& channel(int number);
    std::vector<int> configureMpeZone(int master, int count);
    static void clearNoteOrigin(Channel& channel, const Origin& origin);
    static void copyControllerReset(Channel& channel, const Channel& source, const Origin& origin,
                                    bool resetTimbre = false);
    void rebuildPortNameIndex();
    PortEntry* findPort(const JsString& id);
    const PortEntry* findPort(const JsString& id) const;
    HostObject channelJson(const Channel& channel, bool full) const;
    HostObject stateJson(bool full) const;

    std::array<Channel, 16> m_channels;
    double m_clockCount = 0.0;
    bool m_registry = true;
    std::vector<PortEntry> m_ports;                     // insertion-ordered Map keyed by id
    std::vector<std::pair<JsString, JsString>> m_portsByName; // name -> port id ("" = ambiguous)
    std::optional<std::vector<std::pair<JsString, JsString>>> m_portInventory; // name -> id ("" = ambiguous)
    std::unique_ptr<MidiState> m_unscoped;
    std::optional<int> m_mpeLower;
    std::optional<int> m_mpeUpper;
};

} // namespace nm
