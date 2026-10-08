// check_func_bodies_differential.mjs — differential-corpus gate.
// Generates a deterministic corpus of arrow-function bodies, decides each
// body three ways, and fails on any disagreement:
//   1. REFERENCE (this node's V8): `new Function('state', body)` throws.
//   2. CANDIDATE (C++ checker): nm::js::checkFunctionBody via the
//      nm-dump js-syntax batch helper (build it first).
//   3. SECOND MAJOR (optional, NM_EXTRA_NODE=<node binary>): the same
//      reference probe under another node major, when one is available.
// The reference and the candidate must agree on every body; Unsupported
// (U) from the candidate also fails -- a corpus body the checker cannot
// decide is a regression in decision power.
//
// Raw results are RETAINED: the encoded corpus and the per-body verdicts of
// every deciding process are written to parity/evidence/func_bodies_*.log
// (review correction 2026-09-25: the earlier run's raw result was not
// retained, so the explicit acceptance check stayed open).
//
// Run once per node major, then diff the two evidence logs' VERDICT-REF
// sections to show the majors agree:
//   NM_JSYNTAX_DUMP=build/nm-dump-js-syntax NM_EXTRA_NODE=<node24> \
//     node parity/check_func_bodies_differential.mjs
import { createHash } from 'node:crypto'
import { execFileSync, spawnSync } from 'node:child_process'
import { mkdirSync, readFileSync, writeFileSync, existsSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const REPO = resolve(HERE, '..')
const DUMP = process.env.NM_JSYNTAX_DUMP || join(REPO, 'build', 'nm-dump-js-syntax')
const EXTRA_NODE = process.env.NM_EXTRA_NODE || ''
const EVIDENCE = process.env.NM_DIFF_EVIDENCE_DIR || join(HERE, 'evidence')

if (!existsSync(DUMP)) {
    console.error(`js_syntax_dump not found at ${DUMP} (build it first, or set NM_JSYNTAX_DUMP=<path>)`)
    process.exit(2)
}

// ---------------------------------------------------------------------------
// Deterministic corpus generation (xorshift32; the corpus is regenerated
// identically on every run, and its digest is stamped into the evidence log).
function rng(seed) {
    let s = seed >>> 0
    return () => {
        s ^= s << 13; s >>>= 0
        s ^= s >> 17
        s ^= s << 5; s >>>= 0
        return s
    }
}

const IDENT_START = 'abcdefghijklmnopqrstuvwxyz_ABC$_'
const IDENT_REST = IDENT_START + '0123456789'
const NONS = ['\u200C', '\u200D']
const ACCENTS = ['\u00E9', '\u0130', '\u1E9E', '\u03C9', '\u4E2D']

function makeIdent(rand, depth) {
    if (depth <= 0 || (rand() & 15) === 0) {
        return 'state'
    }
    let out = ''
    const n = 1 + (rand() % 8)
    for (let i = 0; i < n; ++i) {
        const start = i === 0 ? IDENT_START : IDENT_REST
        out += start[rand() % start.length]
        if ((rand() & 7) === 0) out += NONS[rand() % NONS.length]
        if ((rand() & 15) === 0) out += ACCENTS[rand() % ACCENTS.length]
    }
    return out
}

const UNARY = ['!', '~', '+', '-', 'void ', 'typeof ', 'delete ']
const BINARY = ['+', '-', '*', '/', '%', '**', '==', '===', '!=', '!==', '<', '>',
    '<=', '>=', '&', '|', '^', '&&', '||', '??', '<<', '>>', '>>>', 'in', 'instanceof']
const ASSIGN_OPS = ['=', '+=', '-=', '*=', '/=', '**=', '<<=', '>>=', '&=', '|=', '&&=', '||=', '??=']
const NUMS = ['0', '1', '42', '0.5', '.5', '1e3', '1E-3', '0xf', '0o17', '0b101', '1_000',
    '1.5e2_1', '0n', '123n', '3.14', '1e308', '5e-324', '9007199254740993', '.0e+0']
const STRINGS = [`'a'`, `"b\u00e9"`, `'\\n'`, `'\\x41'`, `'\u{1F600}'`, `'\\0'`, `'\\u0041'`,
    '`t${x}`', '`\\`q\\``', "'\\\\'", '"\\u2028"', `'end\\'`, "'\\8'", "'\\9'", "'\\01'"]
const REGEXES = ['/a/', '/a/gimsy', '/[\\d-\\u0041]/', '/(?<n>\\w)\\k<n>/', '/a+/u', '/(?<n>a)/u',
    '/\\p{Script=Greek}/u', '/[\\p{L}]/u', '/a{2,3}/', '/\\u{41}/', '/\\u{110000}/', '/(/',
    '/a/x', '/a/uy', '/\\q{ab}/v', '/[\\q{ab}]/v', '/[[a]]/v']
const EXPR_TERMS = [null, 'l', 'n', 's', 'r', 'o', 'f']

// Generate one expression-ish source. depth>1 recurses; `body` governs
// statement-form generation (the reference wraps the source in
// `with(state){ return SRC; }`, so the accepted statement forms are
// expression bodies only -- but the corpus pins some near-misses too).
function genExpr(rand, depth, ctx) {
    const pick = a => a[rand() % a.length]
    const kind = rand() % (depth <= 1 ? 6 : 14)
    switch (kind) {
        case 0: return pick(NUMS)
        case 1: return pick(STRINGS)
        case 2: return pick(REGEXES)
        case 3: return makeIdent(rand, 2)
        case 4: return pick(UNARY) + genExpr(rand, depth - 1, ctx)
        case 5: return `${genExpr(rand, depth - 1, ctx)} ${pick(BINARY)} ${genExpr(rand, depth - 1, ctx)}`
        case 6: return `(${genExpr(rand, depth - 1, ctx)})`
        case 7: return `(${genExpr(rand, depth - 1, ctx)}) ? (${genExpr(rand, depth - 1, ctx)}) : (${genExpr(rand, depth - 1, ctx)})`
        case 8: return `{${Array.from({ length: 1 + (rand() % 3) }, () =>
            `${makeIdent(rand, 1)}${(rand() & 1) ? '' : `: ${genExpr(rand, depth - 1, ctx)}`}`).join(', ')}}`
        case 9: return `[${Array.from({ length: 1 + (rand() % 4) }, () =>
            (rand() & 3) === 0 ? `...${genExpr(rand, depth - 1, ctx)}` : genExpr(rand, depth - 1, ctx)).join(', ')}]`
        case 10: return `x => ${genExpr(rand, depth - 1, ctx)}`
        case 11: {
            const head = ['const', 'let', 'var', 'using'][rand() % 4]
            const form = rand() % 3
            const init = form === 0 ? 'x = 1' : form === 1 ? '[a, b = 2] = xs' : '{p: q.c = 3} = xs'
            return `(function(){ for (${head} ${init};;); })`
        }
        case 12: return `${makeIdent(rand, 1)}${pick(ASSIGN_OPS)}${genExpr(rand, depth - 1, ctx)}`
        case 13: return `async ${genExpr(rand, depth - 1, ctx)}`
    }
}

function genCorpus(count) {
    const rand = rng(0x4e4d3137)
    const out = []
    for (let i = 0; i < count; ++i) {
        // Every third body is one of the pinned tricky forms (line/paragraph
        // separators in strings, `in` at expression-head, `yield`,
        // HTML-comment close, `#x in obj` private names, nested for-heads).
        if (i % 3 === 0) {
            const tricky = [
                "'\\u2028\\u2029'", '"a\u2028b"', 'x in y', '(0, x in y)',
                'yield', 'x /* */ in', 'a #b in', 'z \\u2028 => z',
                '[a, ...b]', '({a = 1} = {})', 'class {}', 'function(){}',
                '(function() { for (const x of xs) y })', '`\\u{10FFFF}`',
                '(() => {})', 'new.target', 'import.meta', 'await x',
                '"\\u{D800}"', "f(...a, ...b)", '({...xs})', '(a, b) => (a, b)',
            ]
            out.push(tricky[(rand() % tricky.length) | 0])
        } else {
            out.push(genExpr(rand, 1 + (rand() % 3), { body: true }))
        }
    }
    return out
}

// ---------------------------------------------------------------------------
const COUNT = Number(process.env.NM_DIFF_COUNT || 60000)
if (!Number.isFinite(COUNT) || COUNT <= 0) {
    console.error('NM_DIFF_COUNT must be a positive integer')
    process.exit(2)
}
const sources = genCorpus(COUNT)
const bodies = sources.map(src => `with(state){ return ${src}; }`)
// The wire protocol is per-UTF-16-code-unit: a printable non-% ASCII unit
// goes through literally; any other unit is sent as %XXXX (four hex digits
// of the unit value). The C++ decoder in js_syntax_dump.cpp and the
// extra-node decoder read %XXXX back as one unit, so astral characters and
// non-latin1 BMP units round-trip exactly.
const encode = s => {
    let out = ''
    for (let i = 0; i < s.length; ++i) {
        const u = s.charCodeAt(i)
        if (u >= 0x21 && u <= 0x7e && u !== 0x25) out += String.fromCharCode(u)
        else out += '%' + u.toString(16).toUpperCase().padStart(4, '0')
    }
    return out
}
const corpusText = bodies.map(encode).join('\n') + '\n'
const corpusDigest = createHash('sha256').update(corpusText).digest('hex')

// Reference verdicts under this node major.
const refVerdict = []
for (const body of bodies) {
    try { new Function('state', body); refVerdict.push('V') } catch { refVerdict.push('I') }
}

// Second major (optional).
let extraVerdict = null
let extraVersion = ''
if (EXTRA_NODE) {
    const probe = spawnSync(EXTRA_NODE, ['-e', 'console.log(process.version)'], { encoding: 'utf8' })
    if (probe.status !== 0) {
        console.error(`NM_EXTRA_NODE probe failed: ${probe.stderr}`)
        process.exit(2)
    }
    extraVersion = probe.stdout.trim()
    // The extra node feeds its own verdicts back through the same wire
    // protocol: one 'V'/'I' per line, order preserved.
    const program = `
const lines = require('fs').readFileSync(0, 'utf8').split('\\n').filter(l => l.length)
let out = ''
for (const line of lines) {
  const body = decode(line)
  let v
  try { new Function('state', body); v = 'V' } catch { v = 'I' }
  out += v + '\\n'
}
process.stdout.write(out)
function decode(line) {
  const units = []
  for (let i = 0; i < line.length; ) {
    if (line[i] === '%') { units.push(parseInt(line.slice(i + 1, i + 5), 16)); i += 5 }
    else { units.push(line.charCodeAt(i)); i += 1 }
  }
  return String.fromCharCode.apply(null, units)
}
`
    const run = spawnSync(EXTRA_NODE, ['-e', program],
        { encoding: 'utf8', input: corpusText, maxBuffer: 1 << 28 })
    if (run.status !== 0 || run.stderr) {
        console.error(`extra node failed: ${run.stderr}`)
        process.exit(2)
    }
    extraVerdict = run.stdout.split(/\r?\n/).filter(l => l.length)
}

// Candidate verdicts (C++ checker).
const candRun = spawnSync(DUMP, [], { encoding: 'utf8', input: corpusText, maxBuffer: 1 << 28 })
if (candRun.status !== 0 || candRun.stderr) {
    console.error(`js_syntax_dump failed (exit ${candRun.status}): ${candRun.stderr}`)
    process.exit(2)
}
const candVerdict = candRun.stdout.split(/\r?\n/).filter(l => l.length)
if (candVerdict.length !== bodies.length || refVerdict.length !== bodies.length ||
    (extraVerdict && extraVerdict.length !== bodies.length)) {
    console.error('verdict line count mismatch -- aborting (no evidence written)')
    process.exit(2)
}

// Compare.
const diffs = []
let unsupported = 0
for (let i = 0; i < bodies.length; ++i) {
    if (candVerdict[i] === 'U') { unsupported += 1; diffs.push(i) ; continue }
    if (candVerdict[i] !== refVerdict[i]) diffs.push(i)
    if (extraVerdict && extraVerdict[i] !== refVerdict[i]) diffs.push(i)
}

mkdirSync(EVIDENCE, { recursive: true })
const tag = `${process.version}${extraVersion ? `-and-${extraVersion}` : ''}`
const stamp = new Date().toISOString()
const header = [
    `# differential corpus raw result`,
    `# stamp: ${stamp}`,
    `# this node: ${process.version}`,
    `# extra node: ${extraVersion || '(none)'}`,
    `# candidate binary: ${DUMP}`,
    `# corpus bodies: ${bodies.length}`,
    `# corpus sha256 (encoded bodies, LF, trailing newline): ${corpusDigest}`,
    `# candidate verdicts (one char per body, order preserved):`,
]
const refHeader = [
    `# reference verdicts (${process.version}, one char per body):`,
]
writeFileSync(join(EVIDENCE, `func_bodies_differential_${tag}.log`),
    [...header, candVerdict.join(''), ...refHeader, refVerdict.join(''),
     ...(extraVerdict ? [`# reference verdicts (${extraVersion}, one char per body):`, extraVerdict.join('')] : []),
     ''].join('\n'))
writeFileSync(join(EVIDENCE, 'func_bodies_differential_corpus.txt'), corpusText)

const refCounts = refVerdict.reduce((m, v) => (m[v] = (m[v] || 0) + 1, m), {})
console.log(`DIFFERENTIAL: ${bodies.length - diffs.length}/${bodies.length} agree (${JSON.stringify(refCounts)}, candidate U: ${unsupported})`)
if (diffs.length) {
    for (const i of diffs.slice(0, 10)) {
        console.error(`  DIFF at #${i}: src=${JSON.stringify(sources[i])} ref=${refVerdict[i]} cand=${candVerdict[i]} extra=${extraVerdict ? extraVerdict[i] : '-'}`)
    }
    console.error(`DIFFERENTIAL FAIL: ${diffs.length} disagreements`)
    process.exit(1)
}
