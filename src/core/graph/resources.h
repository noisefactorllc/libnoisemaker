#pragma once

#include "core/lang/enums.h"

namespace nm {

struct PassIO {
    JsVector<JsText> inputs;
    JsVector<JsText> outputs;
};

struct TexLifetime {
    int start = 0;
    int end = 0;
};

JsMap<JsText, TexLifetime> analyzeLiveness(const JsVector<PassIO>& passes);
JsObject allocateResources(const JsVector<PassIO>& passes);

}  // namespace nm
