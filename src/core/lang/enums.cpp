#include "enums.h"

namespace nm {

namespace {

// share/palettes.json key order (verbatim, 0-based positional enum; "none"
// IS index 0; 56 entries) -- verified directly against the live reference
// (NM_REFERENCE_ROOT checkout's share/palettes.json key order via `node
// -e "console.log(Object.keys(JSON.parse(...)))"`), matching godot's
// enums.gd PALETTE_KEYS transcription exactly. std_enums.js builds
// `paletteEnum` by walking `Object.keys(palettes)` in this exact order.
const JsList& paletteKeys() {
    static const JsList keys = {
        JsText(u"none"),          JsText(u"seventiesShirt"), JsText(u"fiveG"),
        JsText(u"afterimage"),    JsText(u"barstow"),        JsText(u"bloob"),
        JsText(u"blueSkies"),     JsText(u"brushedMetal"),   JsText(u"burningSky"),
        JsText(u"california"),    JsText(u"columbia"),      JsText(u"cottonCandy"),
        JsText(u"darkSatin"),     JsText(u"dealerHat"),     JsText(u"dreamy"),
        JsText(u"eventHorizon"),  JsText(u"ghostly"),       JsText(u"grayscale"),
        JsText(u"hazySunset"),    JsText(u"heatmap"),       JsText(u"hypercolor"),
        JsText(u"jester"),        JsText(u"justBlue"),      JsText(u"justCyan"),
        JsText(u"justGreen"),     JsText(u"justPurple"),    JsText(u"justRed"),
        JsText(u"justYellow"),    JsText(u"mars"),          JsText(u"modesto"),
        JsText(u"moss"),          JsText(u"neptune"),       JsText(u"netOfGems"),
        JsText(u"organic"),       JsText(u"papaya"),        JsText(u"radioactive"),
        JsText(u"royal"),         JsText(u"santaCruz"),     JsText(u"sherbet"),
        JsText(u"sherbetDouble"), JsText(u"silvermane"),    JsText(u"skykissed"),
        JsText(u"solaris"),       JsText(u"spooky"),        JsText(u"springtime"),
        JsText(u"sproingtime"),   JsText(u"sulphur"),       JsText(u"summoning"),
        JsText(u"superhero"),     JsText(u"toxic"),         JsText(u"tropicalia"),
        JsText(u"tungsten"),      JsText(u"vaporwave"),     JsText(u"vibrant"),
        JsText(u"vintage"),       JsText(u"vintagePhoto"),
    };
    return keys;
}

JsObject buildStd() {
    JsObject root;

    JsObject channel;
    channel.insert(JsText(u"r"), Enums::leaf(0));
    channel.insert(JsText(u"g"), Enums::leaf(1));
    channel.insert(JsText(u"b"), Enums::leaf(2));
    channel.insert(JsText(u"a"), Enums::leaf(3));
    root.insert(JsText(u"channel"), channel);

    JsObject color;
    color.insert(JsText(u"mono"), Enums::leaf(0));
    color.insert(JsText(u"rgb"), Enums::leaf(1));
    color.insert(JsText(u"hsv"), Enums::leaf(2));
    root.insert(JsText(u"color"), color);

    JsObject oscType;
    oscType.insert(JsText(u"sine"), Enums::leaf(0));
    oscType.insert(JsText(u"linear"), Enums::leaf(1));
    oscType.insert(JsText(u"sawtooth"), Enums::leaf(2));
    oscType.insert(JsText(u"sawtoothInv"), Enums::leaf(3));
    oscType.insert(JsText(u"square"), Enums::leaf(4));
    oscType.insert(JsText(u"noise1d"), Enums::leaf(5));
    oscType.insert(JsText(u"noise2d"), Enums::leaf(6));
    root.insert(JsText(u"oscType"), oscType);

    // oscKind: 'noise' is an alias of 'noise1d' -- BOTH keys carry value 5
    // (reference std_enums.js oscKindEnum comment: "periodic noise (alias
    // for noise1d)").
    JsObject oscKind;
    oscKind.insert(JsText(u"sine"), Enums::leaf(0));
    oscKind.insert(JsText(u"tri"), Enums::leaf(1));
    oscKind.insert(JsText(u"saw"), Enums::leaf(2));
    oscKind.insert(JsText(u"sawInv"), Enums::leaf(3));
    oscKind.insert(JsText(u"square"), Enums::leaf(4));
    oscKind.insert(JsText(u"noise"), Enums::leaf(5));
    oscKind.insert(JsText(u"noise1d"), Enums::leaf(5));
    oscKind.insert(JsText(u"noise2d"), Enums::leaf(6));
    root.insert(JsText(u"oscKind"), oscKind);

    JsObject midiMode;
    midiMode.insert(JsText(u"noteChange"), Enums::leaf(0));
    midiMode.insert(JsText(u"gateNote"), Enums::leaf(1));
    midiMode.insert(JsText(u"gateVelocity"), Enums::leaf(2));
    midiMode.insert(JsText(u"triggerNote"), Enums::leaf(3));
    midiMode.insert(JsText(u"velocity"), Enums::leaf(4));
    midiMode.insert(JsText(u"cc"), Enums::leaf(5));
    midiMode.insert(JsText(u"cc14"), Enums::leaf(6));
    midiMode.insert(JsText(u"nrpn"), Enums::leaf(7));
    midiMode.insert(JsText(u"pitchBend"), Enums::leaf(8));
    midiMode.insert(JsText(u"pressure"), Enums::leaf(9));
    midiMode.insert(JsText(u"polyPressure"), Enums::leaf(10));
    root.insert(JsText(u"midiMode"), midiMode);
    root.insert(JsText(u"midiZone"), JsObject{{JsText(u"lower"), Enums::leaf(0)}, {JsText(u"upper"), Enums::leaf(1)}});

    JsObject audioBand;
    audioBand.insert(JsText(u"low"), Enums::leaf(0));
    audioBand.insert(JsText(u"mid"), Enums::leaf(1));
    audioBand.insert(JsText(u"high"), Enums::leaf(2));
    audioBand.insert(JsText(u"vol"), Enums::leaf(3));
    audioBand.insert(JsText(u"raw"), Enums::leaf(4));
    root.insert(JsText(u"audioBand"), audioBand);

    JsObject palette;
    const JsList& keys = paletteKeys();
    for (int i = 0; i < keys.size(); ++i) {
        palette.insert(keys.at(i), Enums::leaf(i));
    }
    root.insert(JsText(u"palette"), palette);

    return root;
}

// Copy-on-write recursive tree update: returns a COPY of `node` with a leaf
// installed at `path[idx..]`. At each level, an EXISTING subtree is reused
// (its own children preserved) rather than replaced -- mirrors the
// reference's deepMerge, which recurses into an existing non-leaf target
// object instead of overwriting it, so that (e.g.) registering
// filter.warp.amount after filter.blur.mode does not delete the earlier
// filter.blur subtree. An existing LEAF at an intermediate level (a
// namespace/func/key segment colliding with a choice name) is treated as
// "start fresh" rather than the reference's own stranger behavior of
// recursing into the leaf object itself -- structurally unreachable here,
// since registerChoice is always called with the fixed 4-deep
// [ns,func,key,choiceName] shape (ns/func/key segments are never
// themselves registered as leaves).
JsObject insertLeaf(const JsObject& node, const JsList& path, int idx, const JsValue& value) {
    JsObject out = node;
    if (idx == path.size() - 1) {
        out.insert(path.at(idx), Enums::leaf(value));
        return out;
    }
    const JsValue existing = out.value(path.at(idx));
    const JsObject child = (existing.isObject() && !Enums::isLeaf(existing)) ? existing.toObject() : JsObject();
    out.insert(path.at(idx), insertLeaf(child, path, idx + 1, value));
    return out;
}

} // namespace

Enums::Enums() : std_(buildStd()) {}

JsObject Enums::leaf(const JsValue& value) {
    JsObject o;
    o.insert(JsText(u"type"), JsText(u"Number"));
    o.insert(JsText(u"value"), value);
    return o;
}

bool Enums::isLeaf(const JsValue& node) {
    if (!node.isObject()) return false;
    const JsValue t = node.toObject().value(JsText(u"type"));
    return t.isString() && t.toString() == JsText(u"Number");
}

JsValue Enums::tryGetHead(const JsText& head) const {
    if (project_.contains(head)) return project_.value(head);
    if (std_.contains(head)) return std_.value(head);
    // NOTE: JsValue's own default constructor yields Type::Null, NOT
    // Type::Undefined (verified directly -- only JsObject::value() on a
    // MISSING key, or this explicit tag constructor, produce Undefined).
    // Every "not found" sentinel in this compiler must use this exact
    // form, never a bare JsValue().
    return JsValue(JsValue::Undefined);
}

void Enums::registerChoice(const JsList& path, const JsValue& value) {
    if (path.isEmpty()) return;
    project_ = insertLeaf(project_, path, 0, value);
}

} // namespace nm
