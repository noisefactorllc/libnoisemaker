#include "core/graph/resources.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace nm {
namespace {
bool isGlobal(const JsText& id) { return id.startsWith(u"global_"); }

void touch(JsMap<JsText, TexLifetime>& lifetime, const JsText& id, int index) {
    if (id.empty() || isGlobal(id)) return;
    if (!lifetime.contains(id)) lifetime.insert(id, TexLifetime{index, index});
    else {
        auto& span = lifetime[id];
        span.start = std::min(span.start, index);
        span.end = std::max(span.end, index);
    }
}
}  // namespace

JsMap<JsText, TexLifetime> analyzeLiveness(const JsVector<PassIO>& passes) {
    JsMap<JsText, TexLifetime> lifetime;
    for (std::size_t i = 0; i < passes.size(); ++i) {
        for (const auto& id : passes[i].inputs) touch(lifetime, id, static_cast<int>(i));
        for (const auto& id : passes[i].outputs) touch(lifetime, id, static_cast<int>(i));
    }
    return lifetime;
}

JsObject allocateResources(const JsVector<PassIO>& passes) {
    const auto lifetime = analyzeLiveness(passes);
    JsObject allocations;
    JsVector<std::pair<JsText, int>> freeList;
    int physicalCount = 0;

    for (std::size_t i = 0; i < passes.size(); ++i) {
        const auto& pass = passes[i];
        for (const auto& id : pass.outputs) {
            if (isGlobal(id) || allocations.contains(id)) continue;
            auto free = std::find_if(freeList.begin(), freeList.end(), [i](const auto& slot) {
                return slot.second < static_cast<int>(i);
            });
            if (free == freeList.end()) {
                allocations.insert(id, JsText(u"phys_") + JsText::fromLatin1(std::to_string(physicalCount++).c_str()));
            } else {
                allocations.insert(id, free->first);
                freeList.erase(free);
            }
        }
        JsSet<JsText> released;
        for (const auto& id : pass.inputs) {
            if (id.empty()) throw std::runtime_error("Cannot read properties of undefined (reading 'startsWith')");
            if (released.contains(id)) continue;
            released.insert(id);
            if (isGlobal(id) || !lifetime.contains(id)) continue;
            if (lifetime.value(id).end == static_cast<int>(i) && allocations.contains(id)) {
                freeList.append({allocations.value(id).toString(), static_cast<int>(i)});
            }
        }
    }
    return allocations;
}

}  // namespace nm
