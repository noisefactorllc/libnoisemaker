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

inline const JsString Program = QStringLiteral("Program");
inline const JsString VarAssign = QStringLiteral("VarAssign");
inline const JsString IfStmt = QStringLiteral("IfStmt");
inline const JsString Break = QStringLiteral("Break");
inline const JsString Continue = QStringLiteral("Continue");
inline const JsString Return = QStringLiteral("Return");
inline const JsString Call = QStringLiteral("Call");
inline const JsString Write = QStringLiteral("Write");
inline const JsString Write3D = QStringLiteral("Write3D");
inline const JsString Subchain = QStringLiteral("Subchain");
inline const JsString Read = QStringLiteral("Read");
inline const JsString Read3D = QStringLiteral("Read3D");
inline const JsString Number = QStringLiteral("Number");
inline const JsString String = QStringLiteral("String");
inline const JsString Boolean = QStringLiteral("Boolean");
inline const JsString Color = QStringLiteral("Color");
inline const JsString ArrayLiteral = QStringLiteral("ArrayLiteral");
inline const JsString Func = QStringLiteral("Func");
inline const JsString Ident = QStringLiteral("Ident");
inline const JsString Member = QStringLiteral("Member");
inline const JsString Chain = QStringLiteral("Chain");
inline const JsString OutputRef = QStringLiteral("OutputRef");
inline const JsString SourceRef = QStringLiteral("SourceRef");
inline const JsString VolRef = QStringLiteral("VolRef");
inline const JsString GeoRef = QStringLiteral("GeoRef");
inline const JsString XyzRef = QStringLiteral("XyzRef");
inline const JsString VelRef = QStringLiteral("VelRef");
inline const JsString RgbaRef = QStringLiteral("RgbaRef");
inline const JsString MeshRef = QStringLiteral("MeshRef");
inline const JsString Oscillator = QStringLiteral("Oscillator");
inline const JsString Midi = QStringLiteral("Midi");
inline const JsString Audio = QStringLiteral("Audio");

} // namespace nm::NodeKind

namespace nm::ast {

// {type:'Number', value:double} -- the parse-time constant-folded literal.
inline JsonObject number(double value) {
    JsonObject o;
    o.insert(QStringLiteral("type"), NodeKind::Number);
    o.insert(QStringLiteral("value"), value);
    return o;
}

// A 2-segment Member, used for special-form defaults (oscKind.sine,
// midiMode.velocity).
inline JsonObject memberOf(const LangString& a, const LangString& b) {
    JsonObject o;
    o.insert(QStringLiteral("type"), NodeKind::Member);
    JsonArray path;
    path.append(a);
    path.append(b);
    o.insert(QStringLiteral("path"), path);
    return o;
}

// The {line,col} source location the reference attaches to
// Write/Write3D/Subchain/Read/Read3D/Oscillator/Midi/Audio/ArrayLiteral.
inline JsonObject loc(int line, int col) {
    JsonObject o;
    o.insert(QStringLiteral("line"), line);
    o.insert(QStringLiteral("col"), col);
    return o;
}

} // namespace nm::ast
