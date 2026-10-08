#pragma once

#include "enums.h"

#include <string>

namespace nm {

struct ParamDef {
    JsText name;
    JsText type;
    JsValue defaultValue;
    JsValue enumPath;
    JsValue minValue;
    JsValue maxValue;
    JsValue uniformValue;
    JsValue choicesValue;
    JsValue defaultFromValue;

    bool hasDefault() const { return !defaultValue.isUndefined(); }
    bool hasEnumPath() const { return enumPath.isString(); }
    JsText enumPathString() const { return enumPath.toString(); }
    bool hasMin() const { return !minValue.isUndefined(); }
    double minAsDouble() const { return minValue.toDouble(); }
    bool hasMax() const { return !maxValue.isUndefined(); }
    double maxAsDouble() const { return maxValue.toDouble(); }
    bool hasUniform() const { return !uniformValue.isUndefined(); }
    bool hasChoices() const { return choicesValue.isObject(); }
    JsObject choicesObject() const { return choicesValue.toObject(); }
    bool hasDefaultFrom() const { return defaultFromValue.isString(); }
    JsText defaultFromString() const { return defaultFromValue.toString(); }
};

struct OpSpec {
    JsText name;
    JsText opName;
    JsVector<ParamDef> args;
    JsValue rawSpec;
};

class EffectRegistry {
public:
    // Catalog entries are registered in sorted path order, while fields
    // within each definition keep their JSON declaration order.
    void loadEmbedded();
    void applySetup(const Value& action);
    const OpSpec* getOp(const JsText& opName) const;
    JsObject getEffect(const JsText& key) const;
    Value getEffectValue(const JsString& key) const { return getEffect(JsText(key)).raw(); }
    bool hasEffect(const JsText& key) const;
    bool isStarterOp(const JsText& name) const;
    JsText checkEffectAlias(const JsText& opName) const;
    JsList resolveParamAliases(const JsText& opName, JsObject& kwargs) const;
    Enums& enums() { return enums_; }
    const Enums& enums() const { return enums_; }
    const JsObject& defineMap() const { return defineMap_; }
    const JsList& opNamesInOrder() const { return opOrder_; }
    JsObject dumpSummary() const;
    Value dumpSummaryValue() const { return dumpSummary().raw(); }

private:
    void registerEffect(const std::string& rawJson);
    static bool isStarterDef(const JsObject& def);
    static JsObject opSpecToJson(const OpSpec& spec);

    JsMap<JsText, JsObject> effects_;
    JsMap<JsText, OpSpec> ops_;
    JsList opOrder_;
    JsSet<JsText> starterOps_;
    JsMap<JsText, JsObject> paramAliases_;
    JsMap<JsText, JsText> effectAliases_;
    JsObject defineMap_;
    Enums enums_;
};

} // namespace nm
