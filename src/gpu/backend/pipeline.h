#pragma once

// The reference pipeline executor (shaders/src/runtime/pipeline.js, Pipeline)
// over the WebGPU backend. Methods keep the reference's names and its order of
// backend calls; parity/check_pipeline.mjs compares the device calls and the
// pipeline's state with the reference Pipeline on a recording mock device.
//
// Ported so far: the surfaces and textures (init, compilePrograms, resize,
// createSurfaces, recreateTextures with texture pooling, resolveDimension, the
// volume-size clamp and the MRT format budget), the lifecycle-effect
// bookkeeping with synth/media's onUpdate, and the frame (render with its
// global uniforms, automation values, conditions, repeats, viewports, the
// within-frame ping-pong of surfaces, mip regeneration and the swap). Not
// ported yet: setUniform and the other parameter paths, the MIDI and audio
// states, dispose, renderCubemap, preflight and getAudioInputRequirements.
//
// The graph is the compiler's graph as JSON: passes, programs, textures and
// allocations, with the two Maps as objects (their keys are texture ids, never
// array indices, so the orders agree), and renderSurface.
//
// Outside the port: sinks and presentation (the pipeline here registers no
// CanvasSink); the asyncInit overlays of filter/fibers, filter/scratches and
// filter/strayHair (host inputs, ported with the host's canvas; the reference
// run in Node fails before it draws, so neither side makes a device call);
// effects registered at run time beyond the catalog's (the reference's
// registry takes any, and the hooks here come from the catalog's jsHooks); a
// width or height other than a number; a graph whose passes are not an array
// or whose textures are not an object; texture ids that are array indices
// (they would reorder as object keys); a program id other than a string; a
// surface id that is undefined (a texture spec for a mesh surface no pass
// references); a uniform named __proto__; a global uniform resolution,
// tileOffset or fullResolution that is truthy and not an array or object (the
// reference writes its elements); and MIDI and audio states (automation.h).

#include "gpu/backend/backend.h"

#include <array>

#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace libnoisemaker::backend {

// One record of this.surfaces.
struct Surface {
    // A double-buffered surface: the texture ids it reads and writes. They are
    // undefined (empty) for a mesh surface.
    std::optional<JsString> read;
    std::optional<JsString> write;
    double currentFrame = 0;
    // A mesh surface (mesh0-mesh7): its three data textures and their size.
    bool mesh = false;
    JsString positions;
    JsString normals;
    JsString uvs;
    double width = 0;
    double height = 0;
    // swapBuffers() gives a mesh surface a currentFrame too.
    bool meshCurrentFrame = false;
};

class Pipeline {
public:
    struct Options {
        bool texturePooling = false;
    };

    Pipeline(Value graph, WebGPUBackend& backend, Options options);

    // init(width, height): backend.init(), compilePrograms(), resize().
    void init(double width, double height);
    void compilePrograms();
    void resize(double width, double height);
    // compiler.js recompile() once it has the new graph: createSurfaces(),
    // recreateTextures(collectDefaultUniforms()), initAsyncEffects() and
    // initLifecycleEffects(). Errors propagate; recompile() logs them and
    // returns null.
    void recompile(Value graph);

    void createSurfaces();
    void recreateTextures(const Value& uniforms);
    void updateParameterTextures(const Value& uniforms);
    void refreshMipTargets();
    Value collectDefaultUniforms() const;
    double resolveDimension(const Value& spec, double screenSize, const Value& uniforms);
    void initAsyncEffects();
    void initLifecycleEffects();
    // { pooling, allocations, sharedTextures, textures }
    Value getResourcePlan() const;

    // render(time): one frame at the normalized time. Presentation is outside
    // the port.
    void render(double time);
    void syncTime(double time) { lastTime_ = time; }
    // setTileRegion({ offset, fullResolution, renderScale }) and clearTileRegion().
    void setTileRegion(std::array<double, 2> offset, std::array<double, 2> fullResolution,
                       std::optional<double> renderScale);
    void clearTileRegion();
    void clearSurface(const JsString& name);
    FrameState getFrameState() const;

    const Value& graph() const { return graph_; }
    double width() const { return width_; }
    double height() const { return height_; }
    const std::vector<std::pair<JsString, Surface>>& surfaces() const { return surfaces_; }
    const std::vector<std::optional<JsString>>& mipTargets() const { return mipTargets_; }
    // this._feedbackSurfaces, unset until createSurfaces() runs.
    const std::optional<std::vector<JsString>>& feedbackSurfaces() const { return feedbackSurfaces_; }
    const std::optional<bool>& needsMidiNoteGrid() const { return needsMidiNoteGrid_; }
    const std::vector<std::pair<JsString, JsString>>& textureAliases() const { return textureAliases_; }
    const std::vector<Value>& diagnostics() const { return diagnostics_; }
    const std::vector<JsString>& lifecycleEffects() const { return lifecycleEffects_; }
    const Value& globalUniforms() const { return globalUniforms_; }
    double frameIndex() const { return frameIndex_; }
    double lastTime() const { return lastTime_; }
    double lastPassCount() const { return lastPassCount_; }
    // this.frameReadTextures and this.frameWriteTextures: surface name to the
    // texture id passes read and write this frame.
    using FrameBindings = std::vector<std::pair<JsString, std::optional<JsString>>>;
    const FrameBindings& frameReadTextures() const { return frameReadTextures_; }
    const FrameBindings& frameWriteTextures() const { return frameWriteTextures_; }

private:
    const Value* graphMember(const char16_t* key) const;
    // this.graph.passes when the graph and it are truthy; it must be an array.
    Array* passes();
    const Array* passes() const;
    // this.graph.textures when the graph and it are truthy; it must be an object.
    Object* textures();
    const Object* textures() const;
    Surface* surface(const JsString& name);

    static std::optional<JsString> parseGlobalName(const Value& texId);
    static bool isVolumeSizeUniform(const JsString& name);
    Value clampVolumeSize(const Value& value);
    void clampGraphVolumeSizes();
    void applyMrtFormatBudget();
    void recreateTexturePreserving(const std::optional<JsString>& texId, const Value& spec);
    std::vector<std::pair<JsString, JsString>> buildTexturePoolingPlan() const;
    void releaseRegroupedTextures(const std::vector<std::pair<JsString, JsString>>& previousAliases,
                                  const std::vector<std::pair<JsString, JsString>>& nextAliases);
    void applyTextureAliases();

    void updateGlobalUniforms(double time, double deltaTime);
    void invokeUpdateHooks();
    Value* resolvePassUniforms(Value& pass, double time);
    bool shouldSkipPass(const Value& pass) const;
    double resolveRepeatCount(const Value& pass) const;
    void resolvePassViewport(Value& pass, Value& cacheHolder);
    void adoptIterationBindings(const Value& pass);
    void updateFrameSurfaceBindings(const Value& pass, const FrameState& state);
    void swapBuffers();

    Value graph_;
    WebGPUBackend& backend_;
    bool texturePooling_;
    std::vector<std::pair<JsString, JsString>> textureAliases_;
    std::vector<Value> diagnostics_;
    std::unordered_set<JsString> warnedDimensionFallbacks_;
    std::vector<std::pair<JsString, Surface>> surfaces_;
    std::vector<std::optional<JsString>> mipTargets_;
    double width_ = 0;
    double height_ = 0;
    bool isCompiling_ = false;
    std::optional<std::vector<JsString>> feedbackSurfaces_;
    std::optional<bool> needsMidiNoteGrid_;
    // this._lifecycleEffects (effect keys in insertion order) and
    // this._initLifecycleDone.
    std::vector<JsString> lifecycleEffects_;
    std::unordered_set<JsString> initLifecycleDone_;
    // synth/media's state, which its onInit sets. The reference keeps it on the
    // registry's single effect instance, shared by every pipeline.
    double mediaImageWidth_ = 0;
    double mediaImageHeight_ = 0;

    // The frame.
    Value globalUniforms_ = Value(Object{});
    double frameIndex_ = 0;
    double lastTime_ = 0;
    double lastPassCount_ = 0;
    FrameBindings frameReadTextures_;
    FrameBindings frameWriteTextures_;
    std::optional<std::array<double, 2>> tileOffset_;
    std::optional<std::array<double, 2>> fullResolution_;
    // this._renderScale: undefined until setTileRegion, null after clearTileRegion.
    std::optional<double> renderScale_;
    // The uniforms onUpdate hooks returned this frame, by effect key.
    std::vector<std::pair<JsString, Value>> runtimeUniforms_;
    bool hasRuntimeUniforms_ = false;
    // this._oscillatorPassProxy and this._resolvedUniforms: the reused pass a
    // pass with automation renders as, and the object its uniforms resolve
    // into; the two uniforms objects swap each time the proxy is used, and
    // keep the keys earlier passes resolved, as undefined.
    Value oscillatorPassProxy_;
    Value resolvedUniforms_ = Value(Object{});
};

}  // namespace libnoisemaker::backend
