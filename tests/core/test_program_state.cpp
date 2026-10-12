#include "core/edit/program_state/program_state.h"

#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void test_reassigned_callback_registers_twice() {
    libnoisemaker::EffectRegistry registry;
    libnoisemaker::ProgramState<> state(registry);
    std::vector<int> calls;
    libnoisemaker::ProgramStateCore::Listener callback = [&](libnoisemaker::ProgramStateCore&, const libnoisemaker::Value&) { calls.push_back(1); };
    const auto first = state.on(u"change", callback);
    callback = [&](libnoisemaker::ProgramStateCore&, const libnoisemaker::Value&) { calls.push_back(2); };
    const auto second = state.on(u"change", callback);
    state.emit(u"change");
    check(first != second, "reassigned callback receives a new registration ID");
    check(calls == std::vector<int>({1, 2}), "reassigned callback invokes both registrations");
}

void test_once_registration_can_be_removed_by_id() {
    libnoisemaker::EffectRegistry registry;
    libnoisemaker::ProgramState<> state(registry);
    int calls = 0;
    const auto id = state.once(u"change", [&](libnoisemaker::ProgramStateCore&, const libnoisemaker::Value&) { ++calls; });
    state.off(u"change", id);
    state.emit(u"change");
    check(calls == 0, "off removes a once registration by its returned ID");
}

void test_runtime_error_does_not_stop_dispatch() {
    libnoisemaker::EffectRegistry registry;
    libnoisemaker::ProgramState<> state(registry);
    int later_calls = 0;
    int error_logs = 0;
    libnoisemaker::set_program_state_console([&](const libnoisemaker::JsString& level, const std::vector<libnoisemaker::Value>&) {
        if (level == u"error") ++error_logs;
    });
    state.on(u"change", [](libnoisemaker::ProgramStateCore&, const libnoisemaker::Value&) { throw std::runtime_error("listener failure"); });
    state.on(u"change", [&](libnoisemaker::ProgramStateCore&, const libnoisemaker::Value&) { ++later_calls; });
    bool escaped = false;
    try { state.emit(u"change"); }
    catch (const std::runtime_error&) { escaped = true; }
    libnoisemaker::set_program_state_console({});
    check(!escaped, "runtime error from listener does not escape emit");
    check(later_calls == 1, "runtime error does not stop later listeners");
    check(error_logs == 1, "runtime error is logged once");
}

}  // namespace

int main() {
    test_reassigned_callback_registers_twice();
    test_once_registration_can_be_removed_by_id();
    test_runtime_error_does_not_stop_dispatch();
    if (failures) return 1;
    std::puts("PASS: ProgramState event listeners");
    return 0;
}
