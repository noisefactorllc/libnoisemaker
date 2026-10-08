// dump-registry.mjs — ORACLE for the EffectRegistry parity gate. Builds the reference's load-time
// registry from the SAME effect JSONs the GDScript candidate loads, using the REAL reference stores
// (runtime/registry.js, lang/ops.js, lang/enums.js, lang/paramAliases.js, lang/effectAliases.js)
// and mirroring renderer/canvas.js registerEffectWithRuntime() line-for-line. Dumps
// {ops, enums, paramAliases, effectAliases, effectKeys} as JSON for deep comparison.
//
//   NM_REFERENCE_ROOT=<reference-checkout> node tools/dump-registry.mjs <effectsDir>
//
// Reference-based: imports the upstream stores via NM_REFERENCE_ROOT. canvas.js itself is a browser
// module (DOM/WebGL deps) and cannot be imported in Node, so its registration body is mirrored here
// against the real stores; sanitizeEnumName (4 lines, never fires on the catalog) is copied verbatim
// from canvas.js. Feeding both oracle and candidate the same JSONs isolates the registration LOGIC.
import { resolve, join } from 'node:path'
import { pathToFileURL } from 'node:url'
import { readFileSync, readdirSync } from 'node:fs'

if (!process.env.NM_REFERENCE_ROOT) {
    console.error('NM_REFERENCE_ROOT is not set (point it at a noisemaker reference checkout).')
    process.exit(3)
}
const REF = resolve(process.env.NM_REFERENCE_ROOT)
const { registerEffect, getAllEffects } = await import(pathToFileURL(resolve(REF, 'shaders/src/runtime/registry.js')).href)
const { ops, registerOp } = await import(pathToFileURL(resolve(REF, 'shaders/src/lang/ops.js')).href)
const { mergeIntoEnums } = await import(pathToFileURL(resolve(REF, 'shaders/src/lang/enums.js')).href)
const { registerParamAliases } = await import(pathToFileURL(resolve(REF, 'shaders/src/lang/paramAliases.js')).href)
const { registerEffectAlias } = await import(pathToFileURL(resolve(REF, 'shaders/src/lang/effectAliases.js')).href)

// verbatim from renderer/canvas.js
function isValidIdentifier(name) { return /^[a-zA-Z_][a-zA-Z0-9_]*$/.test(name) }
function sanitizeEnumName(name) {
    let result = name.replace(/\s+(.)/g, (_, c) => c.toUpperCase()).replace(/\s+/g, '')
    result = result.replace(/[^a-zA-Z0-9_]/g, '')
    if (!isValidIdentifier(result)) return null
    return result
}

const effectsDir = resolve(process.argv[2] || join(REF, 'shaders', 'effects'))

// gather "<ns>/<file>.json" sorted — must match the candidate's registration order.
const rels = []
for (const ns of readdirSync(effectsDir, { withFileTypes: true })) {
    if (!ns.isDirectory()) continue
    for (const f of readdirSync(join(effectsDir, ns.name))) {
        if (f.endsWith('.json')) rels.push(`${ns.name}/${f}`)
    }
}
rels.sort()

let mergedEnums = null
const paramAliasesOut = {}
const effectAliasesOut = {}

for (const rel of rels) {
    const instance = JSON.parse(readFileSync(join(effectsDir, rel), 'utf8'))
    const namespace = instance.namespace
    const effectName = instance.func   // effect dir name == func across the catalog

    // ---- mirror registerEffectWithRuntime ----
    registerEffect(instance.func, instance)
    registerEffect(`${namespace}.${instance.func}`, instance)
    registerEffect(`${namespace}/${effectName}`, instance)
    registerEffect(`${namespace}.${effectName}`, instance)

    if (instance.func) {
        const choicesToRegister = {}
        const args = Object.entries(instance.globals || {}).map(([key, spec]) => {
            let enumPath = spec.enum || spec.enumPath
            if (spec.choices && !enumPath) {
                enumPath = `${namespace}.${instance.func}.${key}`
                if (!choicesToRegister[namespace]) choicesToRegister[namespace] = {}
                if (!choicesToRegister[namespace][instance.func]) choicesToRegister[namespace][instance.func] = {}
                choicesToRegister[namespace][instance.func][key] = {}
                for (const [name, val] of Object.entries(spec.choices)) {
                    if (name.endsWith(':')) continue
                    choicesToRegister[namespace][instance.func][key][name] = { type: 'Number', value: val }
                    const sanitized = sanitizeEnumName(name)
                    if (sanitized && sanitized !== name) {
                        choicesToRegister[namespace][instance.func][key][sanitized] = { type: 'Number', value: val }
                    }
                }
            }
            return {
                name: key,
                type: spec.type === 'vec4' ? 'color' : spec.type,
                default: spec.default,
                enum: enumPath,
                enumPath: enumPath,
                min: spec.min,
                max: spec.max,
                uniform: spec.uniform,
                choices: spec.choices
            }
        })
        registerOp(`${namespace}.${instance.func}`, { name: instance.func, args })

        if (Object.keys(choicesToRegister).length > 0) {
            mergedEnums = await mergeIntoEnums(choicesToRegister)
        }
        // Non-empty param-alias maps only (empty maps are inert; the candidate skips them too).
        if (instance.paramAliases && Object.keys(instance.paramAliases).length > 0) {
            registerParamAliases(`${namespace}.${instance.func}`, instance.paramAliases)
            paramAliasesOut[`${namespace}.${instance.func}`] = instance.paramAliases
        }
        if (instance.hidden && instance.deprecatedBy) {
            registerEffectAlias(`${namespace}.${instance.func}`, instance.deprecatedBy)
            effectAliasesOut[`${namespace}.${instance.func}`] = instance.deprecatedBy
        }
    }
}

// effectKeys: every lookup key -> its def's "<ns>.<func>" fingerprint.
const effectKeys = {}
for (const [k, inst] of getAllEffects()) {
    effectKeys[k] = `${inst.namespace}.${inst.func}`
}

// JSON round-trip drops undefined-valued keys (min/max/enum/uniform/...), exactly the fields the
// GDScript candidate omits — so the two serializations line up field-for-field.
const out = JSON.parse(JSON.stringify({
    ops,
    enums: mergedEnums || {},
    paramAliases: paramAliasesOut,
    effectAliases: effectAliasesOut,
    effectKeys
}))
process.stdout.write(JSON.stringify(out))
