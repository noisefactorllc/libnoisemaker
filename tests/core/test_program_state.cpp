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
    nm::EffectRegistry registry;
    nm::ProgramState<> state(registry);
    std::vector<int> calls;
    nm::ProgramStateCore::Listener callback = [&](nm::ProgramStateCore&, const nm::Value&) { calls.push_back(1); };
    const auto first = state.on(u"change", callback);
    callback = [&](nm::ProgramStateCore&, const nm::Value&) { calls.push_back(2); };
    const auto second = state.on(u"change", callback);
    state.emit(u"change");
    check(first != second, "reassigned callback receives a new registration ID");
    check(calls == std::vector<int>({1, 2}), "reassigned callback invokes both registrations");
}

void test_once_registration_can_be_removed_by_id() {
    nm::EffectRegistry registry;
    nm::ProgramState<> state(registry);
    int calls = 0;
    const auto id = state.once(u"change", [&](nm::ProgramStateCore&, const nm::Value&) { ++calls; });
    state.off(u"change", id);
    state.emit(u"change");
    check(calls == 0, "off removes a once registration by its returned ID");
}

void test_runtime_error_does_not_stop_dispatch() {
    nm::EffectRegistry registry;
    nm::ProgramState<> state(registry);
    int later_calls = 0;
    int error_logs = 0;
    nm::set_program_state_console([&](const nm::JsString& level, const std::vector<nm::Value>&) {
        if (level == u"error") ++error_logs;
    });
    state.on(u"change", [](nm::ProgramStateCore&, const nm::Value&) { throw std::runtime_error("listener failure"); });
    state.on(u"change", [&](nm::ProgramStateCore&, const nm::Value&) { ++later_calls; });
    bool escaped = false;
    try { state.emit(u"change"); }
    catch (const std::runtime_error&) { escaped = true; }
    nm::set_program_state_console({});
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
