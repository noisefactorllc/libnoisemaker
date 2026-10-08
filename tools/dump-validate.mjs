// dump-validate.mjs — ORACLE for the validator parity gate. Registers effects/ops/enums/aliases/
// starters from the SAME effect JSONs the candidate loads, mirroring tools/export-graph.mjs (the
// real graph-producing path: starters by the pass-rule, NOT def.starter), then dumps the REFERENCE
// validate(parse(lex(src))) output as JSON { absPath: {ok, out} } for each DSL.
//
//   NM_REFERENCE_ROOT=<reference-checkout> node tools/dump-validate.mjs <effectsDir> <file...>
import { resolve, join } from 'node:path'
import { pathToFileURL } from 'node:url'
import { readFileSync, readdirSync } from 'node:fs'

if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }
const REF = resolve(process.env.NM_REFERENCE_ROOT)
const { lex } = await import(pathToFileURL(resolve(REF, 'shaders/src/lang/lexer.js')).href)
const { parse } = await import(pathToFileURL(resolve(REF, 'shaders/src/lang/parser.js')).href)
const { validate, registerStarterOps } = await import(pathToFileURL(resolve(REF, 'shaders/src/lang/validator.js')).href)
const { registerEffect } = await import(pathToFileURL(resolve(REF, 'shaders/src/runtime/registry.js')).href)
const { registerOp } = await import(pathToFileURL(resolve(REF, 'shaders/src/lang/ops.js')).href)
const { mergeIntoEnums } = await import(pathToFileURL(resolve(REF, 'shaders/src/lang/enums.js')).href)
const { registerParamAliases } = await import(pathToFileURL(resolve(REF, 'shaders/src/lang/paramAliases.js')).href)
const { registerEffectAlias } = await import(pathToFileURL(resolve(REF, 'shaders/src/lang/effectAliases.js')).href)

function isValidIdentifier(n) { return /^[a-zA-Z_][a-zA-Z0-9_]*$/.test(n) }
function sanitizeEnumName(name) {
    let r = name.replace(/\s+(.)/g, (_, c) => c.toUpperCase()).replace(/\s+/g, '').replace(/[^a-zA-Z0-9_]/g, '')
    return isValidIdentifier(r) ? r : null
}
// export-graph.mjs starter rule: starter iff no pass consumes an upstream-surface input.
const SENT = ['inputTex', 'inputTex3d', 'src', 'o0', 'o1']
function isStarterDef(inst) {
    return !((inst.passes || []).some(p => p.inputs && Object.values(p.inputs).some(v => SENT.includes(v))))
}

const effectsDir = resolve(process.argv[2])
const files = process.argv.slice(3)

const rels = []
for (const ns of readdirSync(effectsDir, { withFileTypes: true })) {
    if (!ns.isDirectory()) continue
    for (const f of readdirSync(join(effectsDir, ns.name))) if (f.endsWith('.json')) rels.push(`${ns.name}/${f}`)
}
rels.sort()
const starterNames = []
for (const rel of rels) {
    const inst = JSON.parse(readFileSync(join(effectsDir, rel), 'utf8'))
    const namespace = inst.namespace, func = inst.func
    registerEffect(inst.func, inst)
    registerEffect(`${namespace}.${inst.func}`, inst)
    registerEffect(`${namespace}/${func}`, inst)
    registerEffect(`${namespace}.${func}`, inst)
    const choicesToRegister = {}
    const args = Object.entries(inst.globals || {}).map(([key, spec]) => {
        let enumPath = spec.enum || spec.enumPath
        if (spec.choices && !enumPath) {
            enumPath = `${namespace}.${inst.func}.${key}`
            choicesToRegister[namespace] = choicesToRegister[namespace] || {}
            choicesToRegister[namespace][inst.func] = choicesToRegister[namespace][inst.func] || {}
            choicesToRegister[namespace][inst.func][key] = {}
            for (const [n, v] of Object.entries(spec.choices)) {
                if (n.endsWith(':')) continue
                choicesToRegister[namespace][inst.func][key][n] = { type: 'Number', value: v }
                const s = sanitizeEnumName(n); if (s && s !== n) choicesToRegister[namespace][inst.func][key][s] = { type: 'Number', value: v }
            }
        }
        return { name: key, type: spec.type === 'vec4' ? 'color' : spec.type, default: spec.default, enum: enumPath, enumPath, min: spec.min, max: spec.max, uniform: spec.uniform, choices: spec.choices }
    })
    registerOp(`${namespace}.${inst.func}`, { name: inst.func, args })
    if (Object.keys(choicesToRegister).length) await mergeIntoEnums(choicesToRegister)
    if (inst.paramAliases && Object.keys(inst.paramAliases).length) registerParamAliases(`${namespace}.${inst.func}`, inst.paramAliases)
    if (inst.hidden && inst.deprecatedBy) registerEffectAlias(`${namespace}.${inst.func}`, inst.deprecatedBy)
    if (isStarterDef(inst)) starterNames.push(`${namespace}.${inst.func}`)
}
registerStarterOps(starterNames)

const out = {}
for (const f of files) {
    try {
        out[f] = { ok: true, out: validate(parse(lex(readFileSync(f, 'utf8')))) }
    } catch (e) {
        out[f] = { ok: false, error: String(e && e.message || e) }
    }
}
// JSON round-trip drops any embedded functions (Func/Oscillator fn closures), exactly as the
// candidate omits them.
process.stdout.write(JSON.stringify(out))
