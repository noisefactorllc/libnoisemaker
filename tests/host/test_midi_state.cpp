#include "host/midi_state.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {
int failures = 0;
void check(bool condition, const char* label) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

void send(nm::MidiState& state, std::initializer_list<std::uint8_t> bytes,
          const nm::MidiPort* port = nullptr, double time = 1000.0) {
    state.handleMessage(bytes.begin(), bytes.size(), port, time);
}

nm::Object channel(const nm::MidiState& state, const char16_t* number) {
    return state.snapshot().find(u"channels")->as_object().find(number)->as_object();
}
}  // namespace

int main() {
    nm::MidiState state;
    const nm::MidiPort keys{u"port-a", u"Keys", true};
    const nm::MidiPort pads{u"port-b", u"Pads", true};
    send(state, {0xb0, 1, 64}, &keys);
    send(state, {0xb0, 33, 32}, &keys);
    check(channel(state, u"1").find(u"cc14")->as_array()[1].as_number() == ((64 << 7) | 32),
          "14-bit CC combines a pair from one port");
    send(state, {0xb0, 33, 99}, &pads);
    check(channel(state, u"1").find(u"cc14")->as_array()[1].as_number() == 99,
          "aggregate CC14 uses a complete pair from the last port");
    send(state, {0xe0, 0x7f, 0x7f}, &keys);
    check(channel(state, u"1").find(u"pitchBend")->as_number() == 16383,
          "pitch bend stores its full 14-bit value");
    send(state, {0x90, 60, 127}, &keys);
    check(channel(state, u"1").find(u"gate")->as_number() == 1,
          "note on opens the gate");
    send(state, {0x80, 60, 0}, &keys);
    check(channel(state, u"1").find(u"gate")->as_number() == 0,
          "note off closes the gate");
    send(state, {0xb0, 101, 0}, &pads);
    send(state, {0xb0, 100, 6}, &pads);
    send(state, {0xb0, 6, 7}, &pads);
    const auto zoned = state.snapshot();
    const auto& padsState = zoned.find(u"ports")->as_object().find(u"port-b")->as_object()
        .find(u"state")->as_object();
    check(padsState.find(u"mpeZones")->as_object().find(u"lower")->as_number() == 7,
          "RPN 6 sets the source port's lower MPE zone");
    send(state, {0x93, 64, 100}, &pads, 2000.0);
    check(channel(state, u"4").find(u"heldNotes")->as_object().has(u"64"),
          "member-channel note appears in the snapshot");
    send(state, {0xf8});
    send(state, {0xf8}, &keys);
    check(state.clockCount() == 2, "clock pulses count on the aggregate");
    const auto beforeDisconnect = state.snapshot();
    check(beforeDisconnect.has(u"ports") && beforeDisconnect.has(u"unscopedState") &&
          !channel(state, u"1").has(u"ccPorts"),
          "snapshot includes port states without origin bookkeeping");
    state.disconnectPort(u"port-b");
    check(channel(state, u"1").find(u"cc14")->as_array()[1].as_number() == 0,
          "disconnect clears aggregate values supplied by the port");
    check(!channel(state, u"4").find(u"heldNotes")->as_object().has(u"64"),
          "disconnect clears member-channel notes");
    state.reset();
    check(state.clockCount() == 0, "reset clears the clock");
    return failures == 0 ? 0 : 1;
}
