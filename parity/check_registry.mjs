// check_registry.mjs -- EffectRegistry parity gate. Compares the C++
// registry (candidate, via `nm-dump-registry`) against the
// reference registration logic (oracle, via tools/dump-registry.mjs), both
// fed the SAME embedded catalog effect JSONs. Deep value-compare
// per surface (ops / enums / paramAliases / effectAliases / effectKeys);
// report PASS or the first divergence.
//
//   NM_REFERENCE_ROOT=<reference-checkout> NM_REGISTRY_DUMP=build/nm-dump-registry \
//     node parity/check_registry.mjs
//
// Adapted from noisemaker-for-godot/parity/check_registry.mjs: godot spawns
// its candidate via `Godot --headless --script .../_registry_dump.gd`; here
// the candidate is the standalone nm-dump-registry alias.
// One node launch (oracle) + one native launch (candidate). The reference
// remains the sole authority; no TD/Godot dependency.
import { resolve, dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { existsSync } from 'node:fs'
import { execFileSync } from 'node:child_process'

const HERE = dirname(fileURLToPath(import.meta.url))
const REPO = resolve(HERE, '..')
const EFFECTS_DIR = process.env.NM_CATALOG_EFFECTS_DIR || join(REPO, 'build', 'catalog', 'effects')
const REGISTRY_DUMP = process.env.NM_REGISTRY_DUMP || join(REPO, 'build', 'nm-dump-registry')

if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }

if (!existsSync(REGISTRY_DUMP)) {
    console.error(`registry_dump not found at ${REGISTRY_DUMP} (build it first, or set NM_REGISTRY_DUMP=<path>)`)
    process.exit(2)
}

const oracle = JSON.parse(execFileSync('node', [join(REPO, 'tools', 'dump-registry.mjs'), EFFECTS_DIR],
    { encoding: 'utf8', maxBuffer: 1 << 28 }))

const cand = JSON.parse(execFileSync(REGISTRY_DUMP, [EFFECTS_DIR], { encoding: 'utf8', maxBuffer: 1 << 28 }))

// Key-order-insensitive deep equality; arrays remain positionally ordered.
function eq(a, b) {
    if (a === b) return true
    if (a === null || b === null || typeof a !== 'object' || typeof b !== 'object') return a === b
    if (Array.isArray(a) !== Array.isArray(b)) return false
    if (Array.isArray(a)) {
        if (a.length !== b.length) return false
        for (let i = 0; i < a.length; i++) if (!eq(a[i], b[i])) return false
        return true
    }
    const ka = Object.keys(a), kb = Object.keys(b)
    if (ka.length !== kb.length) return false
    for (const k of ka) { if (!(k in b) || !eq(a[k], b[k])) return false }
    return true
}

// First differing path (for diagnostics).
function firstDiff(a, b, path) {
    if (eq(a, b)) return null
    const ta = a === null ? 'null' : Array.isArray(a) ? 'array' : typeof a
    const tb = b === null ? 'null' : Array.isArray(b) ? 'array' : typeof b
    if (ta !== tb || (ta !== 'object' && ta !== 'array')) {
        return `${path}: ref=${JSON.stringify(a)} mine=${JSON.stringify(b)}`
    }
    if (Array.isArray(a)) {
        if (a.length !== b.length) return `${path}: length ref=${a.length} mine=${b.length}`
        for (let i = 0; i < a.length; i++) { const d = firstDiff(a[i], b[i], `${path}[${i}]`); if (d) return d }
        return `${path}: (array differs)`
    }
    const keys = new Set([...Object.keys(a), ...Object.keys(b)])
    for (const k of keys) {
        if (!(k in a)) return `${path}.${k}: missing in ref (mine=${JSON.stringify(b[k])})`
        if (!(k in b)) return `${path}.${k}: missing in mine (ref=${JSON.stringify(a[k])})`
        const d = firstDiff(a[k], b[k], `${path}.${k}`); if (d) return d
    }
    return `${path}: (object differs)`
}

const sections = ['ops', 'enums', 'paramAliases', 'effectAliases', 'effectKeys']
let allPass = true
console.log('REGISTRY PARITY:')
for (const s of sections) {
    const ok = eq(oracle[s], cand[s])
    const refN = oracle[s] ? Object.keys(oracle[s]).length : 0
    const myN = cand[s] ? Object.keys(cand[s]).length : 0
    console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${s.padEnd(14)} (ref ${refN} / mine ${myN} keys)`)
    if (!ok) { allPass = false; console.log('        DIFF ' + firstDiff(oracle[s], cand[s], s)) }
}
console.log(allPass ? `REGISTRY: ${sections.length}/${sections.length}` : `REGISTRY: FAILED`)
process.exit(allPass ? 0 : 1)
