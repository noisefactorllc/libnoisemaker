#pragma once

#include "core/edit/jsv.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace nm::program_state {

void report_emitter_error(const JsString& event, const JsString& error);

// Shared pointer identity corresponds to JavaScript function identity.
template <class C>
using Listener = std::shared_ptr<std::function<void(C&, const Value&)>>;

template <class C, class F>
Listener<C> make_listener(F&& fn) {
    return std::make_shared<std::function<void(C&, const Value&)>>(std::forward<F>(fn));
}

template <class C>
class Emitter {
    struct ListenerSet {
        std::vector<Listener<C>> entries;
        std::size_t iterating = 0;

        void add(const Listener<C>& callback) {
            if (std::find(entries.begin(), entries.end(), callback) == entries.end())
                entries.push_back(callback);
        }
        void erase(const Listener<C>& callback) {
            auto it = std::find(entries.begin(), entries.end(), callback);
            if (it != entries.end()) it->reset();
            compact();
        }
        void compact() {
            if (iterating == 0)
                entries.erase(std::remove(entries.begin(), entries.end(), nullptr), entries.end());
        }
        std::size_t size() const {
            return static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(),
                                                          [](const auto& entry) { return bool(entry); }));
        }
    };
    struct State {
        std::vector<std::pair<JsString, std::shared_ptr<ListenerSet>>> events;

        std::shared_ptr<ListenerSet> find(const JsString& name) const {
            auto it = std::find_if(events.begin(), events.end(), [&](const auto& item) {
                return item.first == name;
            });
            return it == events.end() ? nullptr : it->second;
        }
        void remove(const JsString& name) {
            events.erase(std::remove_if(events.begin(), events.end(), [&](const auto& item) {
                return item.first == name;
            }), events.end());
        }
    };

public:
    Emitter() : state_(std::make_shared<State>()) {}
    Emitter share() const { return *this; }

    void on(const JsString& event, const Listener<C>& callback) {
        auto set = state_->find(event);
        if (!set) {
            set = std::make_shared<ListenerSet>();
            state_->events.emplace_back(event, set);
        }
        set->add(callback);
    }
    void off(const JsString& event, const Listener<C>& callback) {
        auto set = state_->find(event);
        if (set) set->erase(callback);
    }
    Listener<C> once(const JsString& event, const Listener<C>& callback) {
        std::weak_ptr<State> state = state_;
        auto holder = std::make_shared<std::weak_ptr<std::function<void(C&, const Value&)>>>();
        auto wrapper = make_listener<C>([state, holder, event, callback](C& context, const Value& data) {
            if (auto owner = state.lock()) {
                if (auto set = owner->find(event)) {
                    if (auto self = holder->lock()) set->erase(self);
                }
            }
            (*callback)(context, data);
        });
        *holder = wrapper;
        on(event, wrapper);
        return wrapper;
    }
    void remove_all_listeners(const std::optional<JsString>& event = std::nullopt) {
        if (!event || event->empty()) state_->events.clear();
        else state_->remove(*event);
    }
    std::size_t listener_count(const JsString& event) const {
        auto set = state_->find(event);
        return set ? set->size() : 0;
    }
    void emit(C& context, const JsString& event, const Value& data = Value()) {
        auto set = state_->find(event);
        if (!set) return;
        ++set->iterating;
        struct Guard {
            std::shared_ptr<ListenerSet> set;
            ~Guard() { --set->iterating; set->compact(); }
        } guard{set};
        std::size_t index = 0;
        while (index < set->entries.size()) {
            auto callback = set->entries[index++];
            if (!callback) continue;
            try {
                (*callback)(context, data);
            } catch (const JsError& error) {
                report_emitter_error(event, error.name() + u": " + error.message());
            } catch (const std::exception& error) {
                report_emitter_error(event, utf8_to_js(error.what()));
            }
        }
    }

private:
    std::shared_ptr<State> state_;
};

}  // namespace nm::program_state
