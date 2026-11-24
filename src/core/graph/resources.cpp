#include "core/graph/resources.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace libnoisemaker {
namespace {
bool isGlobal(const JsText& id) { return id.startsWith(u"global_"); }

bool isChainOutput(const JsText& id) {
    return id.size() > 9 && id.startsWith(u"node_") && id.ends_with(u"_out")
           && std::all_of(id.begin() + 5, id.end() - 4, [](char16_t c) { return c >= u'0' && c <= u'9'; });
}

using Aliases = JsMap<JsText, JsText>;

Aliases buildInputAliases(const JsVector<PassIO>& passes) {
    JsSet<JsText> written;
    for (const auto& pass : passes) {
        for (const auto& id : pass.outputs) written.insert(id);
    }
    Aliases aliases;
    for (std::size_t i = 0; i < passes.size(); ++i) {
        const auto& pass = passes[i];
        if (pass.nodeId.empty()) continue;
        const JsText ownOutput = pass.nodeId + u"_out";
        for (const auto& id : pass.inputs) {
            if (written.contains(id) || aliases.contains(id)) continue;
            if (id != pass.nodeId + u"_inputColor" && id != pass.nodeId + u"_inputTex") continue;
            // An unwritten pipeline input alias reads the nearest preceding
            // chain output from a different node.
            for (std::size_t j = i; j > 0; --j) {
                const auto& outputs = passes[j - 1].outputs;
                const auto upstream = std::find_if(outputs.begin(), outputs.end(), [&](const JsText& output) {
                    return output != ownOutput && isChainOutput(output);
                });
                if (upstream != outputs.end()) {
                    aliases.insert(id, *upstream);
                    break;
                }
            }
        }
    }
    return aliases;
}

JsText resolveAlias(const Aliases& aliases, const JsText& id) {
    return aliases.contains(id) ? aliases.value(id) : id;
}

void touch(JsMap<JsText, TexLifetime>& lifetime, const JsText& id, int index) {
    if (id.empty() || isGlobal(id)) return;
    if (!lifetime.contains(id)) lifetime.insert(id, TexLifetime{index, index});
    else {
        auto& span = lifetime[id];
        span.start = std::min(span.start, index);
        span.end = std::max(span.end, index);
    }
}
JsMap<JsText, TexLifetime> analyzeLiveness(const JsVector<PassIO>& passes, const Aliases& aliases) {
    JsMap<JsText, TexLifetime> lifetime;
    for (std::size_t i = 0; i < passes.size(); ++i) {
        for (const auto& id : passes[i].inputs) touch(lifetime, resolveAlias(aliases, id), static_cast<int>(i));
        for (const auto& id : passes[i].outputs) touch(lifetime, id, static_cast<int>(i));
    }
    return lifetime;
}
}  // namespace

JsMap<JsText, TexLifetime> analyzeLiveness(const JsVector<PassIO>& passes) {
    return analyzeLiveness(passes, buildInputAliases(passes));
}

JsObject allocateResources(const JsVector<PassIO>& passes) {
    const auto aliases = buildInputAliases(passes);
    const auto lifetime = analyzeLiveness(passes, aliases);
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
        for (const auto& input : pass.inputs) {
            const auto id = resolveAlias(aliases, input);
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

}  // namespace libnoisemaker
