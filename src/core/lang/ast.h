#pragma once
#include "core/lang/compat.h"

// ast.h — DSL AST node `type` strings + shape helpers. Port of the
// REFERENCE shaders/src/lang/parser.js node shapes (source of truth),
// cross-checked against td/noisemaker/compiler/lang/ast.py's NodeKind
// table and godot/addons/noisemaker/compiler/lang/parser.gd's inline
// {"type": "..."} literals.
//
// The reference parser emits plain JS objects discriminated by a `type`
// string (PORTING-GUIDE.md "Compiler porting rules": keep `type` strings
// identical -- the dump gates diff structures, and structure drift is how
// bugs hide). Per that same rule ("AST/graph nodes are JsonObjects"),
// there is no typed C++ class hierarchy here -- AST nodes are JsonObject/
// JsonArray trees built directly in parser.cpp; NodeKind below supplies
// the `type` string constants, and the helpers below build the shapes
// that recur across multiple call sites (numeric constant folding,
// {line,col} locations, 2-segment enum-default members).
//
// PARITY notes (matching ast.py's, since both mirror the same reference):
//   - Number.value is a DOUBLE carrying the parse-time constant fold.
//   - Color.value is a 4-double array in 0..1; no colorspace conversion.
//   - String.value is RAW (backslash escapes not decoded; lexer.cpp).
//   - The top-level chain-statement wrapper has NO `type` key -- it is
//     identified by the presence of its `chain` key (parser.cpp
//     parseStatement's default branch).
//   - Member.path has >= 2 segments; a single segment is an Ident.


namespace nm::NodeKind {

inline const JsString Program = LangString(u"Program");
inline const JsString VarAssign = LangString(u"VarAssign");
inline const JsString IfStmt = LangString(u"IfStmt");
inline const JsString Break = LangString(u"Break");
inline const JsString Continue = LangString(u"Continue");
inline const JsString Return = LangString(u"Return");
inline const JsString Call = LangString(u"Call");
inline const JsString Write = LangString(u"Write");
inline const JsString Write3D = LangString(u"Write3D");
inline const JsString Subchain = LangString(u"Subchain");
inline const JsString Read = LangString(u"Read");
inline const JsString Read3D = LangString(u"Read3D");
inline const JsString Number = LangString(u"Number");
inline const JsString String = LangString(u"String");
inline const JsString Boolean = LangString(u"Boolean");
inline const JsString Color = LangString(u"Color");
inline const JsString ArrayLiteral = LangString(u"ArrayLiteral");
inline const JsString Func = LangString(u"Func");
inline const JsString Ident = LangString(u"Ident");
inline const JsString Member = LangString(u"Member");
inline const JsString Chain = LangString(u"Chain");
inline const JsString OutputRef = LangString(u"OutputRef");
inline const JsString SourceRef = LangString(u"SourceRef");
inline const JsString VolRef = LangString(u"VolRef");
inline const JsString GeoRef = LangString(u"GeoRef");
inline const JsString XyzRef = LangString(u"XyzRef");
inline const JsString VelRef = LangString(u"VelRef");
inline const JsString RgbaRef = LangString(u"RgbaRef");
inline const JsString MeshRef = LangString(u"MeshRef");
inline const JsString Oscillator = LangString(u"Oscillator");
inline const JsString Midi = LangString(u"Midi");
inline const JsString Audio = LangString(u"Audio");

} // namespace nm::NodeKind

namespace nm::ast {

// {type:'Number', value:double} -- the parse-time constant-folded literal.
inline JsonObject number(double value) {
    JsonObject o;
    o.insert(LangString(u"type"), NodeKind::Number);
    o.insert(LangString(u"value"), value);
    return o;
}

// A 2-segment Member, used for special-form defaults (oscKind.sine,
// midiMode.velocity).
inline JsonObject memberOf(const LangString& a, const LangString& b) {
    JsonObject o;
    o.insert(LangString(u"type"), NodeKind::Member);
    JsonArray path;
    path.append(a);
    path.append(b);
    o.insert(LangString(u"path"), path);
    return o;
}

// The {line,col} source location the reference attaches to
// Write/Write3D/Subchain/Read/Read3D/Oscillator/Midi/Audio/ArrayLiteral.
inline JsonObject loc(int line, int col) {
    JsonObject o;
    o.insert(LangString(u"line"), line);
    o.insert(LangString(u"col"), col);
    return o;
}

} // namespace nm::ast
