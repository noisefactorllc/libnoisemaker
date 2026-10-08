#include "core/graph/compat.h"
#include "core/graph/expander.h"
#include "core/value/js_number.h"

#include "core/lang/effect_registry.h"
#include "core/lang/dim.h"
#include "core/catalog/catalog.h"


#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nm {

namespace {

// ---------------------------------------------------------------------
// Small value helpers (reference/03 §1.1 ArgValue / §4.5 stringification)
// ---------------------------------------------------------------------

const JsSet<JsText>& textureArgKinds() {
    // TEXTURE_ARG_KINDS, expander.js line 6. NOT the abbreviated
    // "{temp,output,source,feedback,xyz,vel,rgba}" list reference/03 §4.8
    // itself writes out — the LIVE source's actual Set also has 'vol',
    // 'geo', and 'pipeline' (added by upstream commit ad984822, per TD's
    // expander.py header comment, independently corroborated by reading
    // expander.js directly: `new Set(['temp','output','source','feedback',
    // 'vol','geo','xyz','vel','rgba','pipeline'])`). Missing vol/geo/
    // pipeline here would leak a 3D generator's `source`/`geoSource` (or a
    // heightMap-style `{kind:'pipeline'}` synthesized default) into
    // pass.uniforms as a raw {kind,name} object.
    static const JsSet<JsText> kinds = {
        JsText(u"temp"), JsText(u"output"), JsText(u"source"),
        JsText(u"feedback"), JsText(u"vol"), JsText(u"geo"),
        JsText(u"xyz"), JsText(u"vel"), JsText(u"rgba"),
        JsText(u"pipeline"),
    };
    return kinds;
}

bool isTextureArg(const JsValue& arg) {
    if (!arg.isObject()) return false;
    return textureArgKinds().contains(arg.toObject().value(JsText(u"kind")).toString());
}

// `(isObjectArg && arg.value !== undefined) ? arg.value : arg` — every
// call site in expander.js resolves an arg this same way.
JsValue resolveArgValue(const JsValue& arg) {
    if (arg.isObject()) {
        const JsObject o = arg.toObject();
        if (o.contains(JsText(u"value"))) return o.value(JsText(u"value"));
    }
    return arg;
}

bool isMissing(const JsValue& v) { return v.isUndefined() || v.isNull(); }

// The expander's OWN local `resolveEnum` closure (expander.js lines
// 120-131) walks ONLY the fixed std-enum tree — `let node = stdEnums`,
// never the dynamic per-effect project/choices tree. This is deliberately
// narrower than Validator::resolveEnum (project-before-std); by the time a
// value reaches the expander, any per-effect `choices` member has ALREADY
// been resolved to its integer by the validator, so only fixed std paths
// (e.g. a `member`-typed global's STRING default like "oscKind.sine")
// remain to resolve here.
JsValue resolveEnumStd(const JsObject& stdTree, const JsText& path) {
    JsList parts;
    for (const auto& part : path.split(u'.', false)) parts.append(JsText(part));
    if (parts.isEmpty()) return JsValue(JsValue::Undefined);
    JsValue node = stdTree;
    for (const JsText& part : parts) {
        if (!node.isObject()) return JsValue(JsValue::Undefined);
        const JsObject obj = node.toObject();
        if (!obj.contains(part)) return JsValue(JsValue::Undefined);
        node = obj.value(part);
    }
    if (node.isObject()) {
        const JsObject leaf = node.toObject();
        if (leaf.contains(JsText(u"value"))) return leaf.value(JsText(u"value"));
    }
    return JsValue(JsValue::Undefined);
}

// def.type==='member' && typeof value==='string' -> try resolveEnumStd,
// keep original value if resolution fails (reference/03 §4.5/§4.7/§4.9).
JsValue resolveMemberIfNeeded(const JsObject& stdTree, const JsObject& def, JsValue value) {
    if (def.value(JsText(u"type")).toString() == JsText(u"member") && value.isString()) {
        const JsValue resolved = resolveEnumStd(stdTree, value.toString());
        if (!isMissing(resolved)) return resolved;
    }
    return value;
}

JsText jsStringOf(const JsValue& v) {
    if (v.isBool()) return v.toBool() ? JsText(u"true") : JsText(u"false");
    if (v.isDouble()) return js::numberToString(v.toDouble());
    if (v.isString()) return v.toString();
    if (v.isNull()) return JsText(u"null");
    if (v.isUndefined()) return JsText(u"undefined");
    if (v.isObject()) return JsText(u"[object Object]");
    JsText result;
    const JsArray elements = v.toArray();
    for (std::size_t i = 0; i < elements.size(); ++i) {
        if (i) result.push_back(u',');
        if (!elements[i].isNull() && !elements[i].isUndefined()) result += jsStringOf(elements[i]);
    }
    return result;
}

// Ordered core objects preserve each effect definition's declaration order.
JsList orderedGlobalKeys(EffectRegistry&, const JsText&, const JsObject& globals) {
    return globals.keys();
}

struct PassKeyOrder {
    JsList inputs;
    JsList outputs;
};

bool isParticleTexture(const JsText& name) {
    return name == u"global_xyz" || name == u"global_vel" || name == u"global_rgba" ||
           name == u"global_points_trail" || name == u"global_life_data";
}

} // namespace

// ---------------------------------------------------------------------
// Expander — per-expand() mutable state, mirroring the reference's
// per-plan local variables (expander.js `expand()`'s closure locals;
// TD's `_Expander` class fields, verified to be the same shape).
// ---------------------------------------------------------------------

namespace {

class Expander {
public:
    Expander(EffectRegistry& registry, const JsObject& options)
        : registry_(registry), shaderOverrides_(options.value(JsText(u"shaderOverrides")).toObject()) {}

    ExpandResult run(const JsArray& plans, const JsValue& renderDirective) {
        for (int planIndex = 0; planIndex < plans.size(); ++planIndex) {
            expandPlan(plans.at(planIndex).toObject(), planIndex);
        }
        resolveVolumeHandoffs();
        ExpandResult result;
        if (!isMissing(renderDirective) && renderDirective.isString() && !renderDirective.toString().isEmpty()) {
            result.renderSurface = renderDirective;
        } else if (!lastWrittenSurface_.isEmpty()) {
            result.renderSurface = lastWrittenSurface_;
        } else {
            JsObject err;
            err.insert(JsText(u"message"),
                       JsText(u"No render surface specified and no write() found - add render(oN) or write(oN)"));
            errors_.append(err);
            result.renderSurface = JsValue(JsValue::Null);
        }
        result.passes = passes_;
        result.errors = errors_;
        result.programs = programs_;
        result.textureSpecs = textureSpecs_;
        result.mediaSteps = mediaSteps_;
        return result;
    }

private:
    EffectRegistry& registry_;
    JsObject shaderOverrides_;
    JsVector<ExpandedPass> passes_;
    JsArray errors_;
    JsObject programs_;
    JsObject textureSpecs_;
    GraphMap<JsText, JsText> textureMap_;
    JsText lastWrittenSurface_;
    JsArray mediaSteps_;
    JsSet<JsText> mediaStepIds_;
    // Volume handoffs (reference expander): exported volume -> {param, value} of its
    // producer's sizing uniform; reader sizing scope -> {surface[, writer]}; exported
    // atlas -> source texture.
    GraphMap<JsText, JsObject> writtenVolumes_;
    GraphMap<JsText, JsObject> readVolumes_;
    GraphMap<JsText, JsText> exportedTextures_;
    JsText volumeSizeParam_;

    // per-plan state (reset in expandPlan)
    JsText currentInput_;
    JsText currentInput3d_;
    JsText currentInputGeo_;
    JsText currentInputXyz_;
    JsText currentInputVel_;
    JsText currentInputRgba_;
    JsValue lastInlineWriteTarget_ = JsValue(JsValue::Null);
    JsText currentParticlePipelineId_;
    JsObject pipelineUniforms_;
    JsText chainScopeId_;

    static JsText nodeIdOf(int temp) { return JsText(u"node_%1").arg(temp); }

    JsObject resolveVolume(const JsText& param, JsSet<JsText>& visited) const {
        if (visited.contains(param)) return {};
        visited.insert(param);
        JsObject writer;
        if (readVolumes_.contains(param)) {
            const JsObject read = readVolumes_.value(param);
            writer = read.value(JsText(u"writer")).toObject();
            if (writer.isEmpty()) writer = writtenVolumes_.value(read.value(JsText(u"surface")).toString());
        }
        if (writer.isEmpty() || writer.value(JsText(u"param")).toString() == param) return {};
        const JsObject deeper = resolveVolume(writer.value(JsText(u"param")).toString(), visited);
        return deeper.isEmpty() ? writer : deeper;
    }

    void resolveExport(const JsText& id, JsSet<JsText>& visited) {
        if (visited.contains(id)) return;
        visited.insert(id);
        const JsText source = exportedTextures_.value(id);
        if (source.isEmpty() || source == id) return;
        resolveExport(source, visited);
        if (textureSpecs_.contains(source)) textureSpecs_.insert(id, textureSpecs_.value(source));
    }

    // Follow volume handoffs after expansion so ordering and re-export do not change atlas
    // dimensions. Cycles without a producer retain their defaults.
    void resolveVolumeHandoffs() {
        GraphMap<JsText, JsObject> resolved;
        for (auto it = readVolumes_.cbegin(); it != readVolumes_.cend(); ++it) {
            JsSet<JsText> visited;
            const JsObject source = resolveVolume(it.key(), visited);
            if (!source.isEmpty()) resolved.insert(it.key(), source);
        }
        for (const JsText& id : exportedTextures_.keys()) {
            JsSet<JsText> visited;
            resolveExport(id, visited);
        }
        for (const JsText& key : textureSpecs_.keys()) {
            JsObject spec = textureSpecs_.value(key).toObject();
            bool changed = false;
            for (const JsText& axis : {JsText(u"width"), JsText(u"height"), JsText(u"depth")}) {
                JsObject dim = spec.value(axis).toObject();
                const JsText param = dim.value(JsText(u"param")).toString();
                if (dim.isEmpty() || !resolved.contains(param)) continue;
                dim.insert(JsText(u"param"), resolved.value(param).value(JsText(u"param")));
                spec.insert(axis, dim);
                changed = true;
            }
            if (changed) textureSpecs_.insert(key, spec);
        }
        for (ExpandedPass& pass : passes_) {
            for (auto it = resolved.cbegin(); it != resolved.cend(); ++it) {
                if (!pass.uniforms.contains(it.key())) continue;
                const JsObject& source = it.value();
                pass.uniforms.remove(it.key());
                pass.uniforms.insert(source.value(JsText(u"param")).toString(), source.value(JsText(u"value")));
                pass.uniforms.insert(JsText(u"volumeSize"), source.value(JsText(u"value")));
                if (pass.scopedParams.value(JsText(u"volumeSize")).toString() == it.key()) {
                    pass.scopedParams.insert(JsText(u"volumeSize"), source.value(JsText(u"param")));
                }
            }
        }
    }

    void ensureBlitProgram() {
        if (programs_.contains(JsText(u"blit"))) return;
        JsObject blit;
        blit.insert(JsText(u"fragment"), JsText(uR"NM(#version 300 es
            precision highp float;
            in vec2 v_texCoord;
            uniform sampler2D src;
            out vec4 fragColor;
            void main() {
                fragColor = texture(src, v_texCoord);
            })NM"));
        blit.insert(JsText(u"wgsl"), JsText(uR"NM(
            struct FragmentInput {
                @builtin(position) position: vec4<f32>,
                @location(0) uv: vec2<f32>,
            }

            @group(0) @binding(0) var src: texture_2d<f32>;
            @group(0) @binding(1) var srcSampler: sampler;

            @fragment
            fn main(in: FragmentInput) -> @location(0) vec4<f32> {
                let uv = vec2<f32>(in.uv.x, 1.0 - in.uv.y);
                return textureSample(src, srcSampler, uv);
            }
        )NM"));
        blit.insert(JsText(u"fragmentEntryPoint"), JsText(u"main"));
        programs_.insert(JsText(u"blit"), blit);
    }

    void registerPassthrough(const JsText& nodeId) {
        if (!currentInput_.isEmpty()) textureMap_.insert(nodeId + JsText(u"_out"), currentInput_);
        if (!currentInput3d_.isEmpty()) textureMap_.insert(nodeId + JsText(u"_out3d"), currentInput3d_);
        if (!currentInputGeo_.isEmpty()) textureMap_.insert(nodeId + JsText(u"_outGeo"), currentInputGeo_);
        if (!currentInputXyz_.isEmpty()) textureMap_.insert(nodeId + JsText(u"_outXyz"), currentInputXyz_);
        if (!currentInputVel_.isEmpty()) textureMap_.insert(nodeId + JsText(u"_outVel"), currentInputVel_);
        if (!currentInputRgba_.isEmpty()) textureMap_.insert(nodeId + JsText(u"_outRgba"), currentInputRgba_);
    }

    JsText scopeParticleTex(const JsText& texName) const {
        if (currentParticlePipelineId_.isEmpty()) return texName;
        if (isParticleTexture(texName)) return texName + u'_' + currentParticlePipelineId_;
        return texName;
    }

    JsText scopeChainTex(const JsText& texName) const {
        const JsText particleResult = scopeParticleTex(texName);
        if (particleResult != texName) return particleResult;
        if (texName.startsWith(JsText(u"global_"))) return texName + u'_' + chainScopeId_;
        return texName;
    }

    ExpandedPass makeBlit(const JsText& id, const JsText& src, const JsText& dst,
                           const JsText& nodeId, int stepTemp, bool hasNodeId) {
        ExpandedPass p;
        p.id = id;
        p.isBlit = true;
        p.program = JsText(u"blit");
        p.inputs.append({JsText(u"src"), src});
        p.outputs.append({JsText(u"color"), dst});
        p.uniforms = JsObject();
        if (hasNodeId) {
            p.nodeId = nodeId;
            p.stepIndex = stepTemp;
        }
        return p;
    }

    void expandPlan(const JsObject& plan, int planIndex) {
        // The reference iterates `plan.chain` with for...of. Branch, Break,
        // Continue and Return plans (control flow) have no chain, so the
        // reference raises this TypeError and expands nothing.
        if (!plan.value(JsText(u"chain")).isArray()) {
            throw std::runtime_error("plan.chain is not iterable");
        }
        currentInput_.clear();
        currentInput3d_.clear();
        currentInputGeo_.clear();
        currentInputXyz_.clear();
        currentInputVel_.clear();
        currentInputRgba_.clear();
        lastInlineWriteTarget_ = JsValue(JsValue::Null);
        currentParticlePipelineId_.clear();
        pipelineUniforms_ = JsObject();
        chainScopeId_ = JsText(u"chain_%1").arg(planIndex);
        volumeSizeParam_ = JsText(u"volumeSize_") + chainScopeId_;

        const JsArray chain = plan.value(JsText(u"chain")).toArray();
        for (int stepPos = 0; stepPos < chain.size(); ++stepPos) {
            const JsObject step = chain.at(stepPos).toObject();
            const bool builtin = step.value(JsText(u"builtin")).toBool(false);
            const JsText op = step.value(JsText(u"op")).toString();
            const JsObject stepArgs = step.value(JsText(u"args")).toObject();
            const int temp = step.value(JsText(u"temp")).toInt();
            const JsText nodeId = nodeIdOf(temp);

            if (builtin && op == JsText(u"_read")) {
                const JsValue tex = stepArgs.value(JsText(u"tex"));
                if (tex.isObject() && tex.toObject().value(JsText(u"kind")).toString() == JsText(u"output")) {
                    currentInput_ = JsText(u"global_") + tex.toObject().value(JsText(u"name")).toString();
                }
                textureMap_.insert(nodeId + JsText(u"_out"), currentInput_);
                continue;
            }
            if (builtin && op == JsText(u"_read3d")) {
                const JsValue tex3d = stepArgs.value(JsText(u"tex3d"));
                const JsValue geo = stepArgs.value(JsText(u"geo"));
                if (tex3d.isObject()) {
                    const JsObject t = tex3d.toObject();
                    const JsText kind = t.value(JsText(u"kind")).toString();
                    const JsText type = t.value(JsText(u"type")).toString();
                    if (kind == JsText(u"vol") || type == JsText(u"VolRef")) {
                        currentInput3d_ = JsText(u"global_") + t.value(JsText(u"name")).toString();
                    } else {
                        const JsValue nameVal = t.value(JsText(u"name"));
                        currentInput3d_ = nameVal.isString() ? nameVal.toString() : JsText();
                    }
                }
                if (geo.isObject()) {
                    const JsObject g = geo.toObject();
                    const JsText kind = g.value(JsText(u"kind")).toString();
                    const JsText type = g.value(JsText(u"type")).toString();
                    if (kind == JsText(u"geo") || type == JsText(u"GeoRef")) {
                        currentInputGeo_ = JsText(u"global_") + g.value(JsText(u"name")).toString();
                    } else {
                        const JsValue nameVal = g.value(JsText(u"name"));
                        currentInputGeo_ = nameVal.isString() ? nameVal.toString() : JsText();
                    }
                }
                // Resolve the producer scope after all plans have been expanded: readers may
                // precede writers to consume the previous frame. Preserve the writer visible
                // at this read; a later filter may rewrite the surface without owning its size.
                if (!currentInput3d_.isEmpty()) {
                    JsObject read;
                    read.insert(JsText(u"surface"), currentInput3d_);
                    JsValue size = 64;
                    if (writtenVolumes_.contains(currentInput3d_)) {
                        const JsObject writer = writtenVolumes_.value(currentInput3d_);
                        read.insert(JsText(u"writer"), writer);
                        size = writer.value(JsText(u"value"));
                    }
                    readVolumes_.insert(volumeSizeParam_, read);
                    pipelineUniforms_.insert(JsText(u"volumeSize"), size);
                    pipelineUniforms_.insert(volumeSizeParam_, size);
                }
                if (!currentInput3d_.isEmpty()) textureMap_.insert(nodeId + JsText(u"_out3d"), currentInput3d_);
                if (!currentInputGeo_.isEmpty()) textureMap_.insert(nodeId + JsText(u"_outGeo"), currentInputGeo_);
                continue;
            }
            if (builtin && op == JsText(u"_write")) {
                const JsValue tex = stepArgs.value(JsText(u"tex"));
                if (tex.isObject() && !currentInput_.isEmpty()) {
                    const JsObject t = tex.toObject();
                    const JsText name = t.value(JsText(u"name")).toString();
                    if (name != JsText(u"none")) {
                        const JsText target = JsText(u"global_") + name;
                        if (currentInput_ != target) {
                            passes_.append(makeBlit(nodeId + JsText(u"_write_blit"), currentInput_, target, nodeId, temp, true));
                            ensureBlitProgram();
                            lastWrittenSurface_ = name;
                            JsObject twt;
                            twt.insert(JsText(u"kind"), t.value(JsText(u"kind")));
                            twt.insert(JsText(u"name"), name);
                            lastInlineWriteTarget_ = twt;
                        }
                    }
                    textureMap_.insert(nodeId + JsText(u"_out"), currentInput_);
                }
                continue;
            }
            if (builtin && op == JsText(u"_write3d")) {
                const JsValue tex3d = stepArgs.value(JsText(u"tex3d"));
                const JsValue geo = stepArgs.value(JsText(u"geo"));
                if (tex3d.isObject()) {
                    const JsText name = tex3d.toObject().value(JsText(u"name")).toString();
                    if (name != JsText(u"none") && !currentInput3d_.isEmpty()) {
                        const JsText targetVol = JsText(u"global_") + name;
                        exportedTextures_.insert(targetVol, currentInput3d_);
                        if (textureSpecs_.contains(currentInput3d_)) {
                            textureSpecs_.insert(targetVol, textureSpecs_.value(currentInput3d_));
                        }
                        if (pipelineUniforms_.contains(JsText(u"volumeSize"))) {
                            JsObject writer;
                            writer.insert(JsText(u"param"), volumeSizeParam_);
                            writer.insert(JsText(u"value"), pipelineUniforms_.value(JsText(u"volumeSize")));
                            writtenVolumes_.insert(targetVol, writer);
                        }
                        if (currentInput3d_ != targetVol) {
                            passes_.append(makeBlit(nodeId + JsText(u"_write3d_vol_blit"), currentInput3d_, targetVol, nodeId, temp, true));
                            ensureBlitProgram();
                        }
                    }
                }
                if (geo.isObject()) {
                    const JsText name = geo.toObject().value(JsText(u"name")).toString();
                    if (name != JsText(u"none") && !currentInputGeo_.isEmpty()) {
                        const JsText targetGeo = JsText(u"global_") + name;
                        exportedTextures_.insert(targetGeo, currentInputGeo_);
                        if (textureSpecs_.contains(currentInputGeo_)) {
                            textureSpecs_.insert(targetGeo, textureSpecs_.value(currentInputGeo_));
                        }
                        if (currentInputGeo_ != targetGeo) {
                            passes_.append(makeBlit(nodeId + JsText(u"_write3d_geo_blit"), currentInputGeo_, targetGeo, nodeId, temp, true));
                        }
                    }
                }
                textureMap_.insert(nodeId + JsText(u"_out"), currentInput_);
                textureMap_.insert(nodeId + JsText(u"_out3d"), currentInput3d_);
                textureMap_.insert(nodeId + JsText(u"_outGeo"), currentInputGeo_);
                continue;
            }
            if (builtin && (op == JsText(u"_subchain_begin") || op == JsText(u"_subchain_end"))) {
                registerPassthrough(nodeId);
                continue;
            }

            lastInlineWriteTarget_ = JsValue(JsValue::Null);

            if (stepArgs.value(JsText(u"_skip")).toBool(false)) {
                registerPassthrough(nodeId);
                continue;
            }

            const JsText effectName = op;
            const JsObject effectDef = registry_.getEffect(effectName);
            if (!registry_.hasEffect(effectName)) {
                JsObject err;
                err.insert(JsText(u"message"), JsText(u"Effect '%1' not found").arg(effectName));
                err.insert(JsText(u"step"), step);
                errors_.append(err);
                continue;
            }

            expandEffectStep(effectDef, step, stepArgs, effectName, nodeId, temp, stepPos, plan);
        }

        // final chain output (reference/03 §4.11)
        const JsValue writeVal = plan.value(JsText(u"write"));
        if (writeVal.isObject() && !currentInput_.isEmpty()) {
            const JsObject w = writeVal.toObject();
            const JsText outName = w.value(JsText(u"name")).toString();
            lastWrittenSurface_ = outName;
            bool alreadyWritten = false;
            if (lastInlineWriteTarget_.isObject()) {
                const JsObject t = lastInlineWriteTarget_.toObject();
                alreadyWritten = t.value(JsText(u"kind")).toString() == JsText(u"output")
                                  && t.value(JsText(u"name")).toString() == outName;
            }
            if (alreadyWritten) return;
            const JsText targetSurface = JsText(u"global_") + outName;
            if (currentInput_ != targetSurface) {
                passes_.append(makeBlit(JsText(u"final_blit_") + outName, currentInput_, targetSurface, JsText(), 0, false));
            }
        }
    }

    // reference/03 §4.4-§4.10 — one real effect step.
    void expandEffectStep(const JsObject& effectDef, const JsObject& step, const JsObject& stepArgs,
                           const JsText& effectName, const JsText& nodeId, int temp, int stepPos, const JsObject& plan) {
        GraphMap<JsText, JsText> scopedParamMap;

        // 1. Particle-pipeline scope detection (§4.4 step 1).
        const JsObject textures = effectDef.value(JsText(u"textures")).toObject();
        if (textures.contains(JsText(u"global_xyz"))) {
            currentParticlePipelineId_ = nodeId;
            currentInputXyz_.clear();
            currentInputVel_.clear();
            currentInputRgba_.clear();
        }

        // 2. Compile-time defines (§4.5) — see file header for the
        // verified GLOBAL-name sort (not define-name).
        JsObject compileTimeDefines;
        JsText programDefineSuffix;
        collectDefines(effectDef, stepArgs, compileTimeDefines, programDefineSuffix);

        // The reference uses the step's override object in place of the
        // effect's shaders, including its program names and source fields.
        const JsText stepKey = JsText::fromLatin1(std::to_string(temp).c_str());
        const JsValue overrideValue = shaderOverrides_.value(stepKey);
        const bool hasOverride = overrideValue.isObject();
        JsObject shadersSource;
        if (hasOverride) {
            shadersSource = overrideValue.toObject();
        } else {
            // Embedded catalog definitions retain shader bytes separately;
            // their pass definitions supply the program names.
            const JsObject effectShaders = effectDef.value(JsText(u"shaders")).toObject();
            for (const JsValue& passValue : effectDef.value(JsText(u"passes")).toArray()) {
                const JsText progName = passValue.toObject().value(JsText(u"program")).toString();
                if (!progName.isEmpty() && !shadersSource.contains(progName)) {
                    shadersSource.insert(progName, effectShaders.value(progName).toObject());
                }
            }
        }
        for (auto it = shadersSource.constBegin(); it != shadersSource.constEnd(); ++it) {
            const JsText progName = it.key();
            const JsText uniqueProgName = nodeId + u'_' + progName + programDefineSuffix;
            if (programs_.contains(uniqueProgName)) continue;
            JsObject entry = it.value().toObject();
            if (!hasOverride) {
                const JsText shaderPath = JsText(u"wgsl/") + effectDef.value(JsText(u"namespace")).toString()
                                          + u'/' + effectDef.value(JsText(u"func")).toString()
                                          + u'/' + progName + JsText(u".wgsl");
                if (const CatalogFile* shader = catalog_find(shaderPath.toStdString())) {
                    entry.insert(JsText(u"wgsl"), JsText::fromUtf8(std::string(shader->bytes)));
                }
            }
            JsValue layout = effectDef.value(JsText(u"uniformLayouts")).toObject().value(progName);
            if (layout.isUndefined() || layout.isNull()) layout = effectDef.value(JsText(u"uniformLayout"));
            if (!layout.isUndefined()) entry.insert(JsText(u"uniformLayout"), layout);
            entry.insert(JsText(u"defines"), compileTimeDefines);
            programs_.insert(uniqueProgName, entry);
        }

        // 4. Texture-spec collection, 2D then 3D (§6).
        collectTextures2d(effectDef, nodeId, scopedParamMap);
        collectTextures3d(effectDef, nodeId);

        // 5. Resolve input cursor from step.from.
        const JsValue fromVal = step.value(JsText(u"from"));
        if (!fromVal.isNull() && !fromVal.isUndefined()) {
            currentInput_ = textureMap_.value(nodeIdOf(fromVal.toInt()) + JsText(u"_out"));
        }

        // 6. Globals -> pipelineUniforms defaults + colorMode (§4.7).
        applyGlobalDefaults(effectDef, stepArgs, effectName);

        // 7/8. Args two passes (§4.8).
        JsSet<JsText> colorModeControlled;
        argsFirstPass(effectDef, stepArgs, colorModeControlled);
        argsSecondPass(effectDef, stepArgs, colorModeControlled);

        // 9. Per-pass expansion (§4.9).
        expandPasses(effectDef, step, stepArgs, effectName, nodeId, temp, stepPos, plan,
                     compileTimeDefines, programDefineSuffix, scopedParamMap);

        // 10. Cursor updates (§4.10).
        updateCursorsAfterPasses(effectDef, nodeId, step.value(JsText(u"from")));
    }

    // reference/03 §4.5. Sorted by GLOBAL name (verified against the live
    // JS source — see file header "PARITY-CRITICAL DEVIATION").
    void collectDefines(const JsObject& effectDef, const JsObject& stepArgs,
                         JsObject& outDefines, JsText& outSuffix) {
        const JsObject globals = effectDef.value(JsText(u"globals")).toObject();
        if (globals.isEmpty()) return;
        JsList sortedGlobalNames = globals.keys();
        std::sort(sortedGlobalNames.begin(), sortedGlobalNames.end());

        JsVector<std::pair<JsText, JsValue>> entries; // (defineName, value), in insertion order
        for (const JsText& globalName : sortedGlobalNames) {
            const JsObject def = globals.value(globalName).toObject();
            const JsValue defineNameVal = def.value(JsText(u"define"));
            if (!defineNameVal.isString()) continue;
            const JsText defineName = defineNameVal.toString();

            JsValue value = def.value(JsText(u"default"));
            if (stepArgs.contains(globalName)) {
                value = resolveArgValue(stepArgs.value(globalName));
            }
            value = resolveMemberIfNeeded(std_(), def, value);
            if (!isMissing(value)) {
                entries.append({defineName, value});
            }
        }
        for (const auto& e : entries) {
            outDefines.insert(e.first, e.second);
            outSuffix += JsText(u"__") + e.first + u'_' + jsStringOf(e.second);
        }
    }

    const JsObject& std_() const { return registry_.enums().std(); }

    void collectTextures2d(const JsObject& effectDef, const JsText& nodeId, GraphMap<JsText, JsText>& scopedParamMap) {
        const JsObject textures = effectDef.value(JsText(u"textures")).toObject();
        for (auto it = textures.constBegin(); it != textures.constEnd(); ++it) {
            const JsText texName = it.key();
            const JsObject spec = it.value().toObject();
            const bool isParticleTex = isParticleTexture(texName);
            const bool shouldScopeAsParticle = isParticleTex && !currentParticlePipelineId_.isEmpty();
            const bool shouldScopeAsChain = texName.startsWith(JsText(u"global_")) && !isParticleTex;

            JsText virtualTexId;
            if (texName.startsWith(JsText(u"global_"))) {
                virtualTexId = shouldScopeAsParticle ? (texName + u'_' + currentParticlePipelineId_)
                                                      : (texName + u'_' + chainScopeId_);
            } else {
                virtualTexId = nodeId + u'_' + texName;
            }

            const bool hasParamRef = dim::referencesParam(spec.value(JsText(u"width")))
                                      || dim::referencesParam(spec.value(JsText(u"height")));
            JsObject resolvedSpec = spec;
            const bool shouldScopeParams = shouldScopeAsParticle || shouldScopeAsChain
                                            || (!currentParticlePipelineId_.isEmpty() && !texName.startsWith(JsText(u"global_")))
                                            || hasParamRef;
            if (shouldScopeParams) {
                const JsText scopeSuffix = shouldScopeAsParticle ? currentParticlePipelineId_ : chainScopeId_;
                // A non-global, non-particle-scoped texture (shouldScopeAsParticle false) that
                // still references `stateSize` (e.g. heightGrid's pass-through/agent programs)
                // must scope THAT param to the particle pipeline id, not this texture's own
                // chain scope — reference expander.js's inline `dimensionScope` ternary.
                const JsText stateSizeScope = (!currentParticlePipelineId_.isEmpty() && !texName.startsWith(JsText(u"global_")))
                                                    ? currentParticlePipelineId_
                                                    : JsText();
                resolvedSpec.insert(JsText(u"width"), dim::scope(spec.value(JsText(u"width")), scopeSuffix, scopedParamMap, stateSizeScope));
                resolvedSpec.insert(JsText(u"height"), dim::scope(spec.value(JsText(u"height")), scopeSuffix, scopedParamMap, stateSizeScope));
            }
            textureSpecs_.insert(virtualTexId, resolvedSpec);
        }
    }

    void collectTextures3d(const JsObject& effectDef, const JsText& nodeId) {
        const JsObject textures3d = effectDef.value(JsText(u"textures3d")).toObject();
        for (auto it = textures3d.constBegin(); it != textures3d.constEnd(); ++it) {
            const JsText texName = it.key();
            const JsText virtualTexId = texName.startsWith(JsText(u"global_")) ? scopeChainTex(texName)
                                                                                        : (nodeId + u'_' + texName);
            JsObject spec = it.value().toObject();
            spec.insert(JsText(u"is3D"), true);
            textureSpecs_.insert(virtualTexId, spec);
        }
    }

    // reference/03 §4.7.
    void applyGlobalDefaults(const JsObject& effectDef, const JsObject& stepArgs, const JsText& effectKey) {
        const JsObject globals = effectDef.value(JsText(u"globals")).toObject();
        const JsList order = orderedGlobalKeys(registry_, effectKey, globals);
        for (const JsText& globalName : order) {
            const JsObject def = globals.value(globalName).toObject();
            const JsValue uniformVal = def.value(JsText(u"uniform"));
            const JsValue defaultVal = def.value(JsText(u"default"));
            if (uniformVal.isString() && !isMissing(defaultVal)) {
                const JsText uniformName = uniformVal.toString();
                if (!pipelineUniforms_.contains(uniformName)) {
                    JsValue val = resolveMemberIfNeeded(std_(), def, defaultVal);
                    pipelineUniforms_.insert(uniformName, val);
                }
            }
            if (def.value(JsText(u"type")).toString() == JsText(u"surface")) {
                const JsValue cmu = def.value(JsText(u"colorModeUniform"));
                if (cmu.isString()) {
                    if (!stepArgs.contains(globalName)) {
                        const bool isNone = defaultVal.isString() && defaultVal.toString() == JsText(u"none");
                        pipelineUniforms_.insert(cmu.toString(), isNone ? 0 : 1);
                    }
                }
            }
        }
    }

    // reference/03 §4.8 first pass.
    void argsFirstPass(const JsObject& effectDef, const JsObject& stepArgs, JsSet<JsText>& colorModeControlled) {
        const JsObject globals = effectDef.value(JsText(u"globals")).toObject();
        for (auto it = stepArgs.constBegin(); it != stepArgs.constEnd(); ++it) {
            const JsValue arg = it.value();
            if (!isTextureArg(arg)) continue;
            const JsObject globalDef = globals.value(it.key()).toObject();
            const JsValue cmu = globalDef.value(JsText(u"colorModeUniform"));
            if (cmu.isString()) {
                const bool isNone = arg.toObject().value(JsText(u"name")).toString() == JsText(u"none");
                pipelineUniforms_.insert(cmu.toString(), isNone ? 0 : 1);
                colorModeControlled.insert(cmu.toString());
            }
        }
    }

    // reference/03 §4.8 second pass.
    void argsSecondPass(const JsObject& effectDef, const JsObject& stepArgs, const JsSet<JsText>& colorModeControlled) {
        const JsObject globals = effectDef.value(JsText(u"globals")).toObject();
        for (auto it = stepArgs.constBegin(); it != stepArgs.constEnd(); ++it) {
            const JsText argName = it.key();
            const JsValue arg = it.value();
            if (isTextureArg(arg)) continue;

            JsText uniformName = argName;
            const JsObject globalDef = globals.value(argName).toObject();
            const JsValue uniformOverride = globalDef.value(JsText(u"uniform"));
            if (uniformOverride.isString()) uniformName = uniformOverride.toString();

            if (colorModeControlled.contains(uniformName)) continue;
            if (uniformName == JsText(u"volumeSize") && !currentInput3d_.isEmpty()
                && pipelineUniforms_.contains(JsText(u"volumeSize"))) {
                continue;
            }
            pipelineUniforms_.insert(uniformName, resolveArgValue(arg));
        }
    }

    void updateCursorsAfterPasses(const JsObject& effectDef, const JsText& nodeId, const JsValue& stepFrom) {
        currentInput_ = textureMap_.value(nodeId + JsText(u"_out"));

        const JsValue outputTexVal = effectDef.value(JsText(u"outputTex"));
        if (outputTexVal.isString() && currentInput_.isEmpty()) {
            const JsText internalTexName = outputTexVal.toString();
            if (internalTexName == JsText(u"inputTex")) {
                // reference/03 §4.10: restore from the previous node's
                // output when this effect's outputTex declaration is
                // itself just "inputTex" (a 2D passthrough effect).
                if (!stepFrom.isNull() && !stepFrom.isUndefined()) {
                    const JsText prevOutKey = nodeIdOf(stepFrom.toInt()) + JsText(u"_out");
                    const JsText prevOutput = textureMap_.value(prevOutKey);
                    if (!prevOutput.isEmpty()) {
                        textureMap_.insert(nodeId + JsText(u"_out"), prevOutput);
                        currentInput_ = prevOutput;
                    }
                }
            } else {
                const JsText virtualTexId = internalTexName.startsWith(JsText(u"global_"))
                                                  ? scopeChainTex(internalTexName)
                                                  : (nodeId + u'_' + internalTexName);
                textureMap_.insert(nodeId + JsText(u"_out"), virtualTexId);
                currentInput_ = virtualTexId;
            }
        }

        const JsText out3d = textureMap_.value(nodeId + JsText(u"_out3d"));
        if (!out3d.isEmpty()) currentInput3d_ = out3d;
        const JsText outXyz = textureMap_.value(nodeId + JsText(u"_outXyz"));
        if (!outXyz.isEmpty()) currentInputXyz_ = outXyz;
        const JsText outVel = textureMap_.value(nodeId + JsText(u"_outVel"));
        if (!outVel.isEmpty()) currentInputVel_ = outVel;
        const JsText outRgba = textureMap_.value(nodeId + JsText(u"_outRgba"));
        if (!outRgba.isEmpty()) currentInputRgba_ = outRgba;

        const JsValue outputTex3dVal = effectDef.value(JsText(u"outputTex3d"));
        if (outputTex3dVal.isString() && out3d.isEmpty()) {
            const JsText internalTexName = outputTex3dVal.toString();
            if (internalTexName == JsText(u"inputTex3d")) {
                if (!currentInput3d_.isEmpty()) textureMap_.insert(nodeId + JsText(u"_out3d"), currentInput3d_);
            } else {
                const JsText virtualTexId = internalTexName.startsWith(JsText(u"global_"))
                                                  ? scopeChainTex(internalTexName)
                                                  : (nodeId + u'_' + internalTexName);
                textureMap_.insert(nodeId + JsText(u"_out3d"), virtualTexId);
                currentInput3d_ = virtualTexId;
            }
        }

        const JsValue outputGeoVal = effectDef.value(JsText(u"outputGeo"));
        if (outputGeoVal.isString()) {
            const JsText geoTexName = outputGeoVal.toString();
            if (geoTexName == JsText(u"inputGeo")) {
                if (!currentInputGeo_.isEmpty()) textureMap_.insert(nodeId + JsText(u"_outGeo"), currentInputGeo_);
            } else {
                const JsText virtualGeoId = nodeId + u'_' + geoTexName;
                textureMap_.insert(nodeId + JsText(u"_outGeo"), virtualGeoId);
                currentInputGeo_ = virtualGeoId;
            }
        }

        applyAgentPassthrough(effectDef, JsText(u"outputXyz"), nodeId, JsText(u"_outXyz"), JsText(u"inputXyz"),
                               outXyz, currentInputXyz_);
        applyAgentPassthrough(effectDef, JsText(u"outputVel"), nodeId, JsText(u"_outVel"), JsText(u"inputVel"),
                               outVel, currentInputVel_);
        applyAgentPassthrough(effectDef, JsText(u"outputRgba"), nodeId, JsText(u"_outRgba"), JsText(u"inputRgba"),
                               outRgba, currentInputRgba_);
    }

    void applyAgentPassthrough(const JsObject& effectDef, const JsText& declKey, const JsText& nodeId,
                                const JsText& outSuffix, const JsText& reuseKeyword, const JsText& already,
                                JsText& cursor) {
        if (!already.isEmpty()) return; // out* already set this step (§4.10 `!outXyz` guard)
        const JsValue declVal = effectDef.value(declKey);
        if (!declVal.isString()) return;
        const JsText texName = declVal.toString();
        if (texName == reuseKeyword) {
            if (!cursor.isEmpty()) textureMap_.insert(nodeId + outSuffix, cursor);
        } else {
            const JsText virtualId = texName.startsWith(JsText(u"global_")) ? scopeChainTex(texName)
                                                                                     : (nodeId + u'_' + texName);
            textureMap_.insert(nodeId + outSuffix, virtualId);
            cursor = virtualId;
        }
    }

    // reference/03 §4.9 — builds each Pass for this effect step.
    void expandPasses(const JsObject& effectDef, const JsObject& step, const JsObject& stepArgs,
                       const JsText& effectName, const JsText& nodeId, int temp, int stepPos, const JsObject& plan,
                       const JsObject& compileTimeDefines, const JsText& programDefineSuffix,
                       GraphMap<JsText, JsText>& scopedParamMap) {
        const JsArray effectPasses = effectDef.value(JsText(u"passes")).toArray();
        const JsObject globals = effectDef.value(JsText(u"globals")).toObject();
        const JsList orderedGlobals = orderedGlobalKeys(registry_, effectName, globals);

        JsVector<PassKeyOrder> keyOrders;
        for (const JsValue& item : effectPasses) {
            const JsObject passDef = item.toObject();
            keyOrders.append({passDef.value(u"inputs").toObject().keys(),
                              passDef.value(u"outputs").toObject().keys()});
        }

        // Uniform names gated by ANY pass's conditions.runIf/skipIf (reference expander.js
        // `conditionalUniforms`). A choice-bearing global normally has NO uniformSpecs entry
        // (percentage-based automation scaling doesn't apply to a discrete selector) — but a
        // conditional selector like viewMode/blendMode/shapeMode must resolve to the SAME
        // integer in every shader pass and in CPU-side pass selection, so it gets one anyway.
        JsSet<JsText> conditionalUniforms;
        for (const JsValue& pv : effectPasses) {
            const JsObject cond = pv.toObject().value(JsText(u"conditions")).toObject();
            for (const JsValue& c : cond.value(JsText(u"runIf")).toArray()) {
                conditionalUniforms.insert(c.toObject().value(JsText(u"uniform")).toString());
            }
            for (const JsValue& c : cond.value(JsText(u"skipIf")).toArray()) {
                conditionalUniforms.insert(c.toObject().value(JsText(u"uniform")).toString());
            }
        }

        for (int i = 0; i < effectPasses.size(); ++i) {
            const JsObject passDef = effectPasses.at(i).toObject();

            ExpandedPass pass;
            pass.id = JsText(u"%1_pass_%2").arg(nodeId).arg(i);
            pass.program = JsText(u"%1_%2%3").arg(nodeId, passDef.value(JsText(u"program")).toString(), programDefineSuffix);
            pass.entryPoint = passDef.value(JsText(u"entryPoint"));
            pass.drawMode = passDef.value(JsText(u"drawMode"));
            pass.drawBuffers = passDef.value(JsText(u"drawBuffers"));
            pass.count = passDef.value(JsText(u"count"));
            pass.countUniform = passDef.value(JsText(u"countUniform"));
            pass.repeat = passDef.value(JsText(u"repeat"));
            pass.blend = passDef.value(JsText(u"blend"));
            pass.conditions = passDef.value(JsText(u"conditions"));
            pass.workgroups = passDef.value(JsText(u"workgroups"));
            pass.storageBuffers = passDef.value(JsText(u"storageBuffers"));
            pass.storageTextures = passDef.value(JsText(u"storageTextures"));
            // Pass-field propagation (reference fa83eeabf): copied verbatim.
            pass.passName = passDef.value(JsText(u"name"));
            pass.passType = passDef.value(JsText(u"type"));
            pass.clear = passDef.value(JsText(u"clear"));
            pass.viewport = passDef.value(JsText(u"viewport"));
            pass.samplerTypes = passDef.value(JsText(u"samplerTypes"));

            // Pass-level compile-time defines (reference/03-era `.flatMap()` per-variant pass
            // cloning: several clones of the same `program` name, each carrying its own
            // `defines`, e.g. pointsBillboardRender's deposit_0/deposit_1/depositDefocus_1/...).
            // Mirrors reference expander.js: append a sorted-key suffix to the program cache id
            // and, on first use, clone the base (node-level) program entry with defines =
            // {...compileTimeDefines, ...passDef.defines}. `pass.passDefines` ALSO carries the
            // resolved value directly (consumed by normalizePass()/toRawPassJson()) rather than
            // requiring every caller to re-derive it from `programs_[pass.program]`.
            if (!compileTimeDefines.isEmpty()) pass.passDefines = compileTimeDefines;
            const JsValue passDefinesVal = passDef.value(JsText(u"defines"));
            if (passDefinesVal.isObject() && !passDefinesVal.toObject().isEmpty()) {
                const JsObject passDefines = passDefinesVal.toObject();
                const JsValue baseProgramVal = programs_.value(pass.program);
                JsList defineKeys = passDefines.keys();
                std::sort(defineKeys.begin(), defineKeys.end());
                JsText passDefineSuffix;
                for (const JsText& k : defineKeys) {
                    passDefineSuffix += JsText(u"__") + k + u'_' + jsStringOf(passDefines.value(k));
                }
                pass.program += passDefineSuffix;
                JsObject mergedDefines = compileTimeDefines;
                for (auto dit = passDefines.constBegin(); dit != passDefines.constEnd(); ++dit) {
                    mergedDefines.insert(dit.key(), dit.value());
                }
                pass.passDefines = mergedDefines;
                if (baseProgramVal.isObject() && !programs_.contains(pass.program)) {
                    JsObject clonedEntry = baseProgramVal.toObject();
                    clonedEntry.insert(JsText(u"defines"), mergedDefines);
                    programs_.insert(pass.program, clonedEntry);
                }
            }

            pass.effectKey = effectName;
            const JsValue funcVal = effectDef.value(JsText(u"func"));
            pass.effectFunc = funcVal.isString() && !funcVal.toString().isEmpty() ? funcVal : JsValue(effectName);
            const JsValue nsVal = effectDef.value(JsText(u"namespace"));
            const JsValue sourceNsVal = effectDef.contains(JsText(u"sourceNamespace"))
                ? effectDef.value(JsText(u"sourceNamespace")) : nsVal;
            pass.effectNamespace = (sourceNsVal.isString() && !sourceNsVal.toString().isEmpty())
                ? sourceNsVal : JsValue(JsValue::Null);
            pass.nodeId = nodeId;
            pass.stepIndex = temp;

            if (!currentInput3d_.isEmpty() && pipelineUniforms_.contains(JsText(u"volumeSize"))) {
                pass.inheritsVolumeSize = true;
            }

            pass.uniforms = pipelineUniforms_;

            // step 5: defaults fill
            for (const JsText& gk : orderedGlobals) {
                const JsObject def = globals.value(gk).toObject();
                const JsValue uniformVal = def.value(JsText(u"uniform"));
                const JsValue defaultVal = def.value(JsText(u"default"));
                if (!uniformVal.isString() || isMissing(defaultVal)) continue;
                const JsText uName = uniformVal.toString();
                if (pass.uniforms.contains(uName)) continue;
                JsValue val = resolveMemberIfNeeded(std_(), def, defaultVal);
                pass.uniforms.insert(uName, val);
                pipelineUniforms_.insert(uName, val);
            }

            // step 6: uniformSpecs
            pass.hasUniformSpecs = true;
            for (const JsText& gk : orderedGlobals) {
                const JsObject def = globals.value(gk).toObject();
                const JsValue uniformVal = def.value(JsText(u"uniform"));
                const JsText uName = uniformVal.isString() ? uniformVal.toString() : gk;
                const JsText type = def.value(JsText(u"type")).toString();
                const bool hasChoices = def.value(JsText(u"choices")).isObject();
                if ((type == JsText(u"float") || type == JsText(u"int")) && !hasChoices) {
                    JsObject range;
                    const JsValue minVal = def.value(JsText(u"min"));
                    const JsValue maxVal = def.value(JsText(u"max"));
                    range.insert(JsText(u"min"), minVal.isDouble() ? minVal : JsValue(0));
                    range.insert(JsText(u"max"), maxVal.isDouble() ? maxVal : JsValue(100));
                    pass.uniformSpecs.insert(uName, range);
                } else if (type == JsText(u"int") && hasChoices && conditionalUniforms.contains(uName)) {
                    JsObject spec;
                    spec.insert(JsText(u"type"), JsText(u"int"));
                    const JsValue minVal = def.value(JsText(u"min"));
                    const JsValue maxVal = def.value(JsText(u"max"));
                    if (minVal.isDouble() && maxVal.isDouble()) {
                        spec.insert(JsText(u"min"), minVal);
                        spec.insert(JsText(u"max"), maxVal);
                    }
                    pass.uniformSpecs.insert(uName, spec);
                }
            }

            // step 7: args -> uniforms
            for (auto it = stepArgs.constBegin(); it != stepArgs.constEnd(); ++it) {
                const JsText argName = it.key();
                const JsValue arg = it.value();
                if (isTextureArg(arg)) continue;

                JsText uniformName = argName;
                const JsObject globalDef = globals.value(argName).toObject();
                const JsValue uniformOverride = globalDef.value(JsText(u"uniform"));
                if (uniformOverride.isString()) uniformName = uniformOverride.toString();

                bool isControlled = false;
                for (auto git = globals.constBegin(); git != globals.constEnd(); ++git) {
                    if (git.value().toObject().value(JsText(u"colorModeUniform")).toString() == uniformName
                        && git.value().toObject().contains(JsText(u"colorModeUniform"))) {
                        isControlled = true;
                        break;
                    }
                }
                if (isControlled) continue;
                if (uniformName == JsText(u"volumeSize") && !currentInput3d_.isEmpty()
                    && pipelineUniforms_.contains(JsText(u"volumeSize"))) {
                    continue;
                }
                const JsValue resolved = resolveArgValue(arg);
                pass.uniforms.insert(uniformName, resolved);
                pipelineUniforms_.insert(uniformName, resolved);
            }

            // step 8: pass-level uniform wiring
            const JsObject passDefUniforms = passDef.value(JsText(u"uniforms")).toObject();
            for (auto it = passDefUniforms.constBegin(); it != passDefUniforms.constEnd(); ++it) {
                const JsText uniformName = it.key();
                // A literal number specializes draws that share a program (e.g. depthMerge's
                // per-clone `runLength`), without exposing internal pass selection as a DSL
                // arg — reference expander.js: `if (typeof globalRef === 'number') { ...; continue }`.
                if (it.value().isDouble()) {
                    pass.uniforms.insert(uniformName, it.value());
                    continue;
                }
                const JsText globalRef = it.value().toString();
                // Record a renamed mapping so runtime parameter updates reach
                // this shader uniform too (reference bd773801, expander.js:
                // `if (globalRef !== uniformName) { ... pass.uniformAliases }`,
                // consumed by runtime/uniform-aliases.js).
                if (!globalRef.isEmpty() && globalRef != uniformName) {
                    pass.uniformAliases.insert(uniformName, globalRef);
                }
                if (pipelineUniforms_.contains(uniformName)) {
                    pass.uniforms.insert(uniformName, pipelineUniforms_.value(uniformName));
                } else if (!globalRef.isEmpty() && pipelineUniforms_.contains(globalRef)) {
                    pass.uniforms.insert(uniformName, pipelineUniforms_.value(globalRef));
                } else if (!globalRef.isEmpty() && globals.contains(globalRef)) {
                    const JsObject gdef = globals.value(globalRef).toObject();
                    const JsValue gdefault = gdef.value(JsText(u"default"));
                    if (!isMissing(gdefault)) {
                        pass.uniforms.insert(uniformName, resolveMemberIfNeeded(std_(), gdef, gdefault));
                    }
                }
            }

            // step 9: palette expansion
            expandPalettes(globals, orderedGlobals, pass);

            // steps 10/11: inputs / outputs
            mapInputs(effectDef, passDef, step, stepArgs, nodeId, plan, i, keyOrders, pass);
            mapOutputs(passDef, nodeId, plan, i, effectPasses.size(), stepPos, keyOrders, pass);

            // step 12: scoped-param propagation
            for (auto spIt = scopedParamMap.constBegin(); spIt != scopedParamMap.constEnd(); ++spIt) {
                const JsText orig = spIt.key();
                const JsText scoped = spIt.value();
                if (pass.uniforms.contains(orig)) {
                    pass.uniforms.insert(scoped, pass.uniforms.value(orig));
                    pipelineUniforms_.insert(scoped, pass.uniforms.value(orig));
                }
            }
            if (!scopedParamMap.isEmpty()) {
                JsObject sp;
                for (auto spIt = scopedParamMap.constBegin(); spIt != scopedParamMap.constEnd(); ++spIt) sp.insert(spIt.key(), spIt.value());
                pass.scopedParams = sp;
            }

            passes_.append(pass);
        }
    }

    void expandPalettes(const JsObject& globals, const JsList& orderedGlobals, ExpandedPass& pass) {
        for (const JsText& gk : orderedGlobals) {
            const JsObject def = globals.value(gk).toObject();
            if (def.value(JsText(u"type")).toString() != JsText(u"palette")) continue;
            const JsValue uniformVal = def.value(JsText(u"uniform"));
            const JsText uName = uniformVal.isString() ? uniformVal.toString() : gk;
            const JsValue indexVal = pass.uniforms.value(uName);
            if (!indexVal.isDouble()) continue;
            if (!std::isfinite(indexVal.toDouble()) || std::trunc(indexVal.toDouble()) != indexVal.toDouble()) {
                throw std::runtime_error("Cannot read properties of undefined (reading 'offset')");
            }
            const JsObject expanded = expandPaletteVectors(static_cast<int>(indexVal.toDouble()));
            if (expanded.isEmpty()) continue;
            for (auto it = expanded.constBegin(); it != expanded.constEnd(); ++it) {
                if (pass.uniforms.contains(it.key())) {
                    pass.uniforms.insert(it.key(), it.value());
                    pipelineUniforms_.insert(it.key(), it.value());
                }
            }
        }
    }

    // reference/07 palette expansion — implemented in expander.cpp
    // directly (small, single caller); table lives inline below.
    static JsObject expandPaletteVectors(int index);

    // reference/03 §5.1
    void mapInputs(const JsObject& effectDef, const JsObject& passDef, const JsObject& step,
                    const JsObject& stepArgs, const JsText& nodeId, const JsObject& plan, int passIndex,
                    const JsVector<PassKeyOrder>& keyOrders, ExpandedPass& pass) {
        const JsObject inputs = passDef.value(JsText(u"inputs")).toObject();
        if (inputs.isEmpty()) return;
        JsList order = (passIndex < keyOrders.size() && keyOrders.at(passIndex).inputs.size() == inputs.size())
                                 ? keyOrders.at(passIndex).inputs
                                 : inputs.keys();

        for (const JsText& uniformName : order) {
            const JsValue texRefVal = inputs.value(uniformName);
            if (!texRefVal.isString()) continue;
            const JsText texRef = texRefVal.toString();

            bool intPrefixOk = false;
            if (texRef.size() >= 2 && texRef.at(0) == u'o') {
                const char16_t c1 = texRef.at(1);
                intPrefixOk = c1 >= u'0' && c1 <= u'9';
            }
            const bool isPipelineInput = texRef == JsText(u"inputTex") || intPrefixOk;

            JsText resolved;
            if (isPipelineInput) {
                resolved = !currentInput_.isEmpty() ? currentInput_ : texRef;
            } else if (texRef == JsText(u"inputTex3d")) {
                resolved = !currentInput3d_.isEmpty() ? currentInput3d_ : texRef;
            } else if (texRef == JsText(u"inputGeo")) {
                resolved = !currentInputGeo_.isEmpty() ? currentInputGeo_ : texRef;
            } else if (texRef == JsText(u"inputXyz")) {
                resolved = !currentInputXyz_.isEmpty() ? currentInputXyz_ : texRef;
            } else if (texRef == JsText(u"inputVel")) {
                resolved = !currentInputVel_.isEmpty() ? currentInputVel_ : texRef;
            } else if (texRef == JsText(u"inputRgba")) {
                resolved = !currentInputRgba_.isEmpty() ? currentInputRgba_ : texRef;
            } else if (texRef == JsText(u"noise")) {
                resolved = JsText(u"global_noise");
            } else if (texRef == JsText(u"midiNoteGrid")) {
                resolved = JsText(u"midiNoteGrid");
            } else if (texRef == JsText(u"feedback") || texRef == JsText(u"selfTex")) {
                const JsValue writeVal = plan.value(JsText(u"write"));
                if (writeVal.isObject()) {
                    const JsObject w = writeVal.toObject();
                    const JsText outName = w.value(JsText(u"name")).toString();
                    const JsText outKind = w.contains(JsText(u"kind")) && w.value(JsText(u"kind")).isString()
                                                 ? w.value(JsText(u"kind")).toString()
                                                 : JsText(u"output");
                    const JsText prefix = outKind == JsText(u"feedback") ? JsText(u"feedback") : JsText(u"global");
                    resolved = prefix + u'_' + outName;
                } else {
                    resolved = !currentInput_.isEmpty() ? currentInput_ : JsText(u"global_inputTex");
                }
            } else if (effectDef.value(JsText(u"externalTexture")).isString()
                       && texRef == effectDef.value(JsText(u"externalTexture")).toString()) {
                resolved = texRef + JsText(u"_step_") + JsText::fromLatin1(std::to_string(step.value(u"temp").toInt()).c_str());
                if (!mediaStepIds_.contains(resolved)) {
                    mediaStepIds_.insert(resolved);
                    JsObject media;
                    media.insert(JsText(u"textureId"), resolved);
                    media.insert(JsText(u"uniform"), uniformName);
                    media.insert(JsText(u"stepIndex"), step.value(JsText(u"temp")));
                    media.insert(JsText(u"effect"), step.value(JsText(u"op")));
                    mediaSteps_.append(media);
                }
            } else if (stepArgs.contains(texRef)) {
                const JsValue arg = stepArgs.value(texRef);
                if (arg.isNull() || arg.isUndefined()) {
                    continue; // intentionally unbound
                }
                if (arg.isObject()) {
                    const JsObject a = arg.toObject();
                    const JsText kind = a.value(JsText(u"kind")).toString();
                    if (kind == JsText(u"temp")) {
                        resolved = textureMap_.value(nodeIdOf(a.value(JsText(u"index")).toInt()) + JsText(u"_out"));
                        if (resolved.isEmpty()) {
                            // JS assigns an undefined property here. It is
                            // omitted by JSON.stringify, but the allocator
                            // still sees it when compiling the graph.
                            pass.inputs.append({uniformName, JsText()});
                            continue;
                        }
                    } else if (kind == JsText(u"pipeline")
                               && (a.value(JsText(u"name")).toString() == JsText(u"inputTex")
                                   || a.value(JsText(u"name")).toString() == JsText(u"inputColor"))) {
                        resolved = !currentInput_.isEmpty() ? currentInput_ : a.value(JsText(u"name")).toString();
                    } else if (kind == JsText(u"output") || kind == JsText(u"source") || kind == JsText(u"vol")
                               || kind == JsText(u"geo") || kind == JsText(u"xyz") || kind == JsText(u"vel")
                               || kind == JsText(u"rgba")) {
                        const JsText name = a.value(JsText(u"name")).toString();
                        resolved = name == JsText(u"none") ? JsText(u"none") : (JsText(u"global_") + name);
                    }
                } else if (arg.isString()) {
                    resolved = resolveGlobalSurfaceRef(arg.toString());
                }
            } else if (effectDef.value(JsText(u"globals")).toObject().value(texRef).toObject().contains(JsText(u"default"))
                       && !isMissing(effectDef.value(JsText(u"globals")).toObject().value(texRef).toObject().value(JsText(u"default")))) {
                const JsValue defaultValJson = effectDef.value(JsText(u"globals")).toObject().value(texRef).toObject().value(JsText(u"default"));
                if (defaultValJson.isString()) {
                    const JsText defaultVal = defaultValJson.toString();
                    if (defaultVal == JsText(u"none")) {
                        resolved = JsText(u"none");
                    } else if (defaultVal == JsText(u"inputTex") || defaultVal == JsText(u"inputColor")) {
                        resolved = !currentInput_.isEmpty() ? currentInput_ : defaultVal;
                    } else if (isSurfaceRef(defaultVal)) {
                        resolved = JsText(u"global_") + defaultVal;
                    } else if (defaultVal.startsWith(JsText(u"global_"))) {
                        resolved = scopeChainTex(defaultVal);
                    } else {
                        resolved = defaultVal;
                    }
                } else {
                    // non-string default for a texRef key with no other match: reference
                    // falls through to the generic branches below since GlobalHasDefault
                    // (JS) only matches STRING defaults implicitly via the branches that
                    // follow (`defaultVal === 'none'` etc. all assume a string). A numeric
                    // default here is not a valid surface ref; fall through to node-local.
                    resolved = nodeId + u'_' + texRef;
                }
            } else if (texRef.startsWith(JsText(u"global_"))) {
                resolved = scopeChainTex(texRef);
            } else if (texRef == JsText(u"outputTex")) {
                resolved = nodeId + JsText(u"_out");
            } else {
                resolved = nodeId + u'_' + texRef;
            }

            if (!resolved.isEmpty()) pass.inputs.append({uniformName, resolved});
        }
    }

    static bool isSurfaceRef(const JsText& s) {
        if (s.empty() || s.back() < u'0' || s.back() > u'7') return false;
        const JsText prefix(s.substr(0, s.size() - 1));
        return prefix == u"o" || prefix == u"vol" || prefix == u"geo" ||
               prefix == u"xyz" || prefix == u"vel" || prefix == u"rgba";
    }

    static JsText resolveGlobalSurfaceRef(const JsText& name) {
        if (name == JsText(u"none")) return name;
        if (name.startsWith(JsText(u"global_"))) return name;
        if (isSurfaceRef(name)) return JsText(u"global_") + name;
        return name;
    }

    // reference/03 §5.2 + §5.3 last-pass-to-surface optimization.
    void mapOutputs(const JsObject& passDef, const JsText& nodeId, const JsObject& plan, int passIndex,
                     int passCount, int stepPos, const JsVector<PassKeyOrder>& keyOrders, ExpandedPass& pass) {
        const JsObject outputs = passDef.value(JsText(u"outputs")).toObject();
        if (outputs.isEmpty()) return;
        JsList order = (passIndex < keyOrders.size() && keyOrders.at(passIndex).outputs.size() == outputs.size())
                                 ? keyOrders.at(passIndex).outputs
                                 : outputs.keys();

        const JsArray chain = plan.value(JsText(u"chain")).toArray();
        const bool isLastStep = stepPos == chain.size() - 1;
        const bool isLastPass = passIndex == passCount - 1;
        const JsValue writeVal = plan.value(JsText(u"write"));

        for (const JsText& attachment : order) {
            const JsValue texRefVal = outputs.value(attachment);
            if (!texRefVal.isString()) continue;
            const JsText texRef = texRefVal.toString();
            JsText virtualTex;

            if (texRef == JsText(u"outputTex")) {
                if (isLastStep && isLastPass && writeVal.isObject()) {
                    const JsObject w = writeVal.toObject();
                    const JsText outName = w.value(JsText(u"name")).toString();
                    const JsText outKind = w.contains(JsText(u"kind")) && w.value(JsText(u"kind")).isString()
                                                 ? w.value(JsText(u"kind")).toString()
                                                 : JsText(u"output");
                    const JsText prefix = outKind == JsText(u"feedback") ? JsText(u"feedback") : JsText(u"global");
                    virtualTex = prefix + u'_' + outName;
                    lastWrittenSurface_ = outName;
                } else {
                    virtualTex = nodeId + JsText(u"_out");
                }
                textureMap_.insert(virtualTex, virtualTex);
                textureMap_.insert(nodeId + JsText(u"_out"), virtualTex);
            } else if (texRef == JsText(u"outputTex3d")) {
                virtualTex = nodeId + JsText(u"_out3d");
                textureMap_.insert(nodeId + JsText(u"_out3d"), virtualTex);
            } else if (texRef == JsText(u"outputXyz")) {
                virtualTex = nodeId + JsText(u"_outXyz");
                textureMap_.insert(nodeId + JsText(u"_outXyz"), virtualTex);
            } else if (texRef == JsText(u"outputVel")) {
                virtualTex = nodeId + JsText(u"_outVel");
                textureMap_.insert(nodeId + JsText(u"_outVel"), virtualTex);
            } else if (texRef == JsText(u"outputRgba")) {
                virtualTex = nodeId + JsText(u"_outRgba");
                textureMap_.insert(nodeId + JsText(u"_outRgba"), virtualTex);
            } else if (texRef == JsText(u"inputTex3d")) {
                virtualTex = !currentInput3d_.isEmpty() ? currentInput3d_ : (nodeId + JsText(u"_inputTex3d"));
            } else if (texRef == JsText(u"inputGeo")) {
                virtualTex = !currentInputGeo_.isEmpty() ? currentInputGeo_ : (nodeId + JsText(u"_inputGeo"));
            } else if (texRef == JsText(u"inputXyz")) {
                virtualTex = !currentInputXyz_.isEmpty() ? currentInputXyz_ : (nodeId + JsText(u"_inputXyz"));
            } else if (texRef == JsText(u"inputVel")) {
                virtualTex = !currentInputVel_.isEmpty() ? currentInputVel_ : (nodeId + JsText(u"_inputVel"));
            } else if (texRef == JsText(u"inputRgba")) {
                virtualTex = !currentInputRgba_.isEmpty() ? currentInputRgba_ : (nodeId + JsText(u"_inputRgba"));
            } else if (texRef.startsWith(JsText(u"global_"))) {
                virtualTex = scopeChainTex(texRef);
            } else if (texRef.startsWith(JsText(u"feedback_"))) {
                virtualTex = texRef;
            } else {
                virtualTex = nodeId + u'_' + texRef;
            }

            pass.outputs.append({attachment, virtualTex});
        }
    }
};

// ---------------------------------------------------------------------
// Palette expansion (reference/03 §7, shaders/src/runtime/
// palette-expansion.js — read in full, table transcribed verbatim
// including the non-round floats per that file's own §7 hazard warning).
// Legacy classicNoisedeck support only; a `type:'palette'` global holds a
// 1-based index. mode: 0=none,1=hsv,2=oklab,3=rgb (already in
// classicNoisedeck convention, NOT the same numbering as the modern
// filter/palette 0=rgb/1=hsv/2=oklab — see palette-expansion.js header).
// ---------------------------------------------------------------------

struct PaletteEntry {
    double amp[3];
    double freq[3];
    double offset[3];
    double phase[3];
    int mode;
};

const PaletteEntry kPalettes[55] = {
    /* 1  seventiesShirt */ {{0.76, 0.88, 0.37}, {1, 1, 1}, {0.93, 0.97, 0.52}, {0.21, 0.41, 0.56}, 3},
    /* 2  fiveG          */ {{0.56851584, 0.7740668, 0.23485267}, {1, 1, 1}, {0.5, 0.5, 0.5}, {0.727029, 0.08039695, 0.10427457}, 3},
    /* 3  afterimage     */ {{0.5, 0.5, 0.5}, {1, 1, 1}, {0.5, 0.5, 0.5}, {0.3, 0.2, 0.2}, 3},
    /* 4  barstow        */ {{0.45, 0.2, 0.1}, {1, 1, 1}, {0.7, 0.2, 0.2}, {0.5, 0.4, 0.0}, 3},
    /* 5  bloob          */ {{0.09, 0.59, 0.48}, {1, 1, 1}, {0.2, 0.31, 0.98}, {0.88, 0.4, 0.33}, 3},
    /* 6  blueSkies      */ {{0.5, 0.5, 0.5}, {1, 1, 1}, {0.1, 0.4, 0.7}, {0.1, 0.1, 0.1}, 3},
    /* 7  brushedMetal   */ {{0.5, 0.5, 0.5}, {1, 1, 1}, {0.5, 0.5, 0.5}, {0.0, 0.1, 0.2}, 3},
    /* 8  burningSky     */ {{0.7259015, 0.7004237, 0.9494409}, {1, 1, 1}, {0.63290054, 0.37883538, 0.29405284}, {0.0, 0.1, 0.2}, 3},
    /* 9  california     */ {{0.94, 0.33, 0.27}, {1, 1, 1}, {0.74, 0.37, 0.73}, {0.44, 0.17, 0.88}, 3},
    /* 10 columbia       */ {{1.0, 0.7, 1.0}, {1, 1, 1}, {1.0, 0.4, 0.9}, {0.4, 0.5, 0.6}, 3},
    /* 11 cottonCandy    */ {{0.51, 0.39, 0.41}, {1, 1, 1}, {0.59, 0.53, 0.94}, {0.15, 0.41, 0.46}, 3},
    /* 12 darkSatin      */ {{0.0, 0.0, 0.51}, {1, 1, 1}, {0.0, 0.0, 0.43}, {0.0, 0.0, 0.36}, 1},
    /* 13 dealerHat      */ {{0.83, 0.45, 0.19}, {1, 1, 1}, {0.79, 0.45, 0.35}, {0.28, 0.91, 0.61}, 3},
    /* 14 dreamy         */ {{0.5, 0.5, 0.5}, {1, 1, 1}, {0.5, 0.5, 0.5}, {0.0, 0.2, 0.25}, 3},
    /* 15 eventHorizon   */ {{0.5, 0.5, 0.5}, {1, 1, 1}, {0.22, 0.48, 0.62}, {0.1, 0.3, 0.2}, 3},
    /* 16 ghostly        */ {{0.02, 0.92, 0.76}, {1, 1, 1}, {0.51, 0.49, 0.51}, {0.71, 0.23, 0.66}, 1},
    /* 17 grayscale      */ {{0.5, 0.5, 0.5}, {2, 2, 2}, {0.5, 0.5, 0.5}, {1.0, 1.0, 1.0}, 3},
    /* 18 hazySunset     */ {{0.79, 0.56, 0.22}, {1, 1, 1}, {0.96, 0.5, 0.49}, {0.15, 0.98, 0.87}, 3},
    /* 19 heatmap        */ {{0.75804377, 0.62868536, 0.2227562}, {1, 1, 1}, {0.35536355, 0.12935615, 0.17060602}, {0.0, 0.25, 0.5}, 3},
    /* 20 hypercolor     */ {{0.79, 0.5, 0.23}, {1, 1, 1}, {0.75, 0.47, 0.45}, {0.08, 0.84, 0.16}, 3},
    /* 21 jester         */ {{0.7, 0.81, 0.73}, {1, 1, 1}, {0.1, 0.22, 0.27}, {0.99, 0.12, 0.94}, 3},
    /* 22 justBlue       */ {{0.5, 0.5, 0.5}, {0.0, 0.0, 1.0}, {0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}, 3},
    /* 23 justCyan       */ {{0.5, 0.5, 0.5}, {0.0, 1.0, 1.0}, {0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}, 3},
    /* 24 justGreen      */ {{0.5, 0.5, 0.5}, {0.0, 1.0, 0.0}, {0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}, 3},
    /* 25 justPurple     */ {{0.5, 0.5, 0.5}, {1.0, 0.0, 1.0}, {0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}, 3},
    /* 26 justRed        */ {{0.5, 0.5, 0.5}, {1.0, 0.0, 0.0}, {0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}, 3},
    /* 27 justYellow     */ {{0.5, 0.5, 0.5}, {1.0, 1.0, 0.0}, {0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}, 3},
    /* 28 mars           */ {{0.74, 0.33, 0.09}, {1, 1, 1}, {0.62, 0.2, 0.2}, {0.2, 0.1, 0.0}, 3},
    /* 29 modesto        */ {{0.56, 0.68, 0.39}, {1, 1, 1}, {0.72, 0.07, 0.62}, {0.25, 0.4, 0.41}, 3},
    /* 30 moss           */ {{0.78, 0.39, 0.07}, {1, 1, 1}, {0.0, 0.53, 0.33}, {0.94, 0.92, 0.9}, 3},
    /* 31 neptune        */ {{0.5, 0.5, 0.5}, {1, 1, 1}, {0.2, 0.64, 0.62}, {0.15, 0.2, 0.3}, 3},
    /* 32 netOfGems      */ {{0.5, 0.5, 0.5}, {1, 1, 1}, {0.64, 0.12, 0.84}, {0.1, 0.25, 0.15}, 3},
    /* 33 organic        */ {{0.42, 0.42, 0.04}, {1, 1, 1}, {0.47, 0.27, 0.27}, {0.41, 0.14, 0.11}, 3},
    /* 34 papaya         */ {{0.65, 0.4, 0.11}, {1, 1, 1}, {0.72, 0.45, 0.08}, {0.71, 0.8, 0.84}, 3},
    /* 35 radioactive    */ {{0.62, 0.79, 0.11}, {1, 1, 1}, {0.22, 0.56, 0.17}, {0.15, 0.1, 0.25}, 3},
    /* 36 royal          */ {{0.5, 0.5, 0.5}, {1, 1, 1}, {0.41, 0.22, 0.67}, {0.2, 0.25, 0.2}, 3},
    /* 37 santaCruz      */ {{0.5, 0.5, 0.5}, {1, 1, 1}, {0.5, 0.5, 0.5}, {0.25, 0.5, 0.75}, 3},
    /* 38 sherbet        */ {{0.6059281, 0.17591387, 0.17166573}, {1, 1, 1}, {0.5224456, 0.3864609, 0.36020845}, {0.0, 0.25, 0.5}, 3},
    /* 39 sherbetDouble  */ {{0.6059281, 0.17591387, 0.17166573}, {2, 2, 2}, {0.5224456, 0.3864609, 0.36020845}, {0.0, 0.25, 0.5}, 3},
    /* 40 silvermane     */ {{0.42, 0.0, 0.0}, {2, 2, 2}, {0.45, 0.5, 0.42}, {0.63, 1.0, 1.0}, 2},
    /* 41 skykissed      */ {{0.5, 0.5, 0.5}, {1, 1, 1}, {0.83, 0.6, 0.63}, {0.3, 0.1, 0.0}, 3},
    /* 42 solaris        */ {{0.5, 0.5, 0.5}, {1, 1, 1}, {0.6, 0.4, 0.1}, {0.3, 0.2, 0.1}, 3},
    /* 43 spooky         */ {{0.46, 0.73, 0.19}, {1, 1, 1}, {0.27, 0.79, 0.78}, {0.27, 0.16, 0.04}, 2},
    /* 44 springtime     */ {{0.67, 0.25, 0.27}, {1, 1, 1}, {0.74, 0.48, 0.46}, {0.07, 0.79, 0.39}, 3},
    /* 45 sproingtime    */ {{0.9, 0.43, 0.34}, {1, 1, 1}, {0.56, 0.69, 0.32}, {0.03, 0.8, 0.4}, 3},
    /* 46 sulphur        */ {{0.73, 0.36, 0.52}, {1, 1, 1}, {0.78, 0.68, 0.15}, {0.74, 0.93, 0.28}, 3},
    /* 47 summoning      */ {{1.0, 0.0, 0.8}, {1, 1, 1}, {0.0, 0.0, 0.0}, {0.0, 0.5, 0.1}, 3},
    /* 48 superhero      */ {{1.0, 0.25, 0.5}, {0.5, 0.5, 0.5}, {0.0, 0.0, 0.25}, {0.5, 0.0, 0.0}, 3},
    /* 49 toxic          */ {{0.5, 0.5, 0.5}, {1, 1, 1}, {0.26, 0.57, 0.03}, {0.0, 0.1, 0.3}, 3},
    /* 50 tropicalia     */ {{0.28, 0.08, 0.65}, {1, 1, 1}, {0.48, 0.6, 0.03}, {0.1, 0.15, 0.3}, 2},
    /* 51 tungsten       */ {{0.65, 0.93, 0.73}, {1, 1, 1}, {0.31, 0.21, 0.27}, {0.43, 0.45, 0.48}, 3},
    /* 52 vaporwave      */ {{0.9, 0.76, 0.63}, {1, 1, 1}, {0.0, 0.19, 0.68}, {0.43, 0.23, 0.32}, 3},
    /* 53 vibrant        */ {{0.78, 0.63, 0.68}, {1, 1, 1}, {0.41, 0.03, 0.16}, {0.81, 0.61, 0.06}, 3},
    /* 54 vintage        */ {{0.97, 0.74, 0.23}, {1, 1, 1}, {0.97, 0.38, 0.35}, {0.34, 0.41, 0.44}, 3},
    /* 55 vintagePhoto   */ {{0.68, 0.79, 0.57}, {1, 1, 1}, {0.56, 0.35, 0.14}, {0.73, 0.9, 0.99}, 3},
};

JsObject Expander::expandPaletteVectors(int index) {
    if (index <= 0 || index > 55) return JsObject();
    const PaletteEntry& e = kPalettes[index - 1];
    auto vec3 = [](const double v[3]) {
        JsArray a;
        a.append(v[0]);
        a.append(v[1]);
        a.append(v[2]);
        return a;
    };
    JsObject out;
    out.insert(JsText(u"paletteOffset"), vec3(e.offset));
    out.insert(JsText(u"paletteAmp"), vec3(e.amp));
    out.insert(JsText(u"paletteFreq"), vec3(e.freq));
    out.insert(JsText(u"palettePhase"), vec3(e.phase));
    out.insert(JsText(u"paletteMode"), e.mode);
    return out;
}

} // namespace

ExpandResult expand(const JsObject& validated, EffectRegistry& registry, const JsObject& options) {
    Expander expander(registry, options);
    const JsArray plans = validated.value(JsText(u"plans")).toArray();
    const JsValue render = validated.value(JsText(u"render"));
    return expander.run(plans, render);
}

Value toRawPassJson(const ExpandedPass& pass) {
    JsObject out;
    out.insert(JsText(u"id"), pass.id);
    out.insert(JsText(u"program"), pass.program);

    if (pass.isBlit) {
        out.insert(JsText(u"type"), JsText(u"render"));
    } else {
        if (!pass.entryPoint.isUndefined()) out.insert(JsText(u"entryPoint"), pass.entryPoint);
        if (!pass.drawMode.isUndefined()) out.insert(JsText(u"drawMode"), pass.drawMode);
        if (!pass.drawBuffers.isUndefined()) out.insert(JsText(u"drawBuffers"), pass.drawBuffers);
        if (!pass.count.isUndefined()) out.insert(JsText(u"count"), pass.count);
        if (!pass.countUniform.isUndefined()) out.insert(JsText(u"countUniform"), pass.countUniform);
        if (!pass.repeat.isUndefined()) out.insert(JsText(u"repeat"), pass.repeat);
        if (!pass.blend.isUndefined()) out.insert(JsText(u"blend"), pass.blend);
        if (!pass.conditions.isUndefined()) out.insert(JsText(u"conditions"), pass.conditions);
        if (!pass.workgroups.isUndefined()) out.insert(JsText(u"workgroups"), pass.workgroups);
        if (!pass.storageBuffers.isUndefined()) out.insert(JsText(u"storageBuffers"), pass.storageBuffers);
        if (!pass.storageTextures.isUndefined()) out.insert(JsText(u"storageTextures"), pass.storageTextures);
        // Pass-field propagation (reference fa83eeabf): the same five
        // fields the reference copies, serialized only when authored.
        if (!pass.passName.isUndefined()) out.insert(JsText(u"name"), pass.passName);
        if (!pass.passType.isUndefined()) out.insert(JsText(u"type"), pass.passType);
        if (!pass.clear.isUndefined()) out.insert(JsText(u"clear"), pass.clear);
        if (!pass.viewport.isUndefined()) out.insert(JsText(u"viewport"), pass.viewport);
        if (!pass.samplerTypes.isUndefined()) out.insert(JsText(u"samplerTypes"), pass.samplerTypes);
    }

    JsObject inputs;
    for (const auto& kv : pass.inputs) {
        if (!kv.second.isEmpty()) inputs.insert(kv.first, kv.second);
    }
    out.insert(JsText(u"inputs"), inputs);
    JsObject outputs;
    for (const auto& kv : pass.outputs) outputs.insert(kv.first, kv.second);
    out.insert(JsText(u"outputs"), outputs);
    out.insert(JsText(u"uniforms"), pass.uniforms);

    if (pass.isBlit) {
        if (!pass.nodeId.isUndefined()) out.insert(JsText(u"nodeId"), pass.nodeId);
        if (!pass.stepIndex.isUndefined()) out.insert(JsText(u"stepIndex"), pass.stepIndex);
    } else {
        out.insert(JsText(u"effectKey"), pass.effectKey);
        out.insert(JsText(u"effectFunc"), pass.effectFunc);
        out.insert(JsText(u"effectNamespace"), pass.effectNamespace);
        out.insert(JsText(u"nodeId"), pass.nodeId);
        out.insert(JsText(u"stepIndex"), pass.stepIndex);
        if (pass.inheritsVolumeSize) out.insert(JsText(u"inheritsVolumeSize"), true);
        if (pass.hasUniformSpecs) out.insert(JsText(u"uniformSpecs"), pass.uniformSpecs);
        if (!pass.uniformAliases.isEmpty()) out.insert(JsText(u"uniformAliases"), pass.uniformAliases);
        if (!pass.scopedParams.isEmpty()) out.insert(JsText(u"scopedParams"), pass.scopedParams);
    }
    return out.raw();
}

} // namespace nm
