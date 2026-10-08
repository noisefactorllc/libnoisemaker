// check_obj_parser.mjs -- OBJ parser parity gate.
//
// Parses the same OBJ inputs with the REFERENCE parser (shaders/src/runtime/
// obj-parser.js, imported unchanged under Node) and with this port's
// nm::parseOBJ / nm::loadOBJ (build/obj_parser_dump), then compares
// every Float32 of the parsed positions/normals/uvs, the 256x256 packed
// texture arrays (SHA-256 of their bytes), and the 4x4 packed arrays
// (truncation path). NaN compares as NaN; every other value compares by bit
// pattern.
//
// Inputs: the built-in meshes, hand-written edge cases, invalid UTF-8
// files, and seeded random OBJ-like texts. "string" inputs mirror the
// reference parseOBJ(text); "file" inputs mirror loadOBJ(url), whose text
// comes from Response.text() (UTF-8, BOM dropped, U+FFFD replacement).
//
// The gate also checks that reference built-in meshes as parser inputs.
//
//   NM_REFERENCE_ROOT=/path/to/noisemaker node parity/check_obj_parser.mjs
//
// Env: NM_OBJ_PARSER_DUMP  candidate binary (default build/obj_parser_dump)
//      NM_OBJ_FUZZ_CASES   seeded random inputs (default 400)

import { resolve, dirname, join } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { existsSync, mkdtempSync, writeFileSync, readFileSync, readdirSync, rmSync } from 'node:fs'
import { endianness, tmpdir } from 'node:os'
import { execFileSync } from 'node:child_process'
import { createHash } from 'node:crypto'

const HERE = dirname(fileURLToPath(import.meta.url))
const REPO = resolve(HERE, '..')
const DUMP = process.env.NM_OBJ_PARSER_DUMP || join(REPO, 'build', 'nm-dump-obj-parser')
const FUZZ_CASES = Number(process.env.NM_OBJ_FUZZ_CASES || 400)

if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }
if (!existsSync(DUMP)) { console.error(`obj_parser_dump not found at ${DUMP} (build the candidate first)`); process.exit(3) }

const REF = resolve(process.env.NM_REFERENCE_ROOT)
const { parseOBJ, packMeshDataForTextures } =
  await import(pathToFileURL(join(REF, 'shaders/src/runtime/obj-parser.js')).href)

let failures = 0
const fail = (message) => { failures++; console.log(`FAIL ${message}`) }

// -------------------------------------------------------- built-in inputs
// libnoisemaker does not ship mesh assets; parse the reference built-ins as
// fixture inputs. Asset identity belongs to the product repositories.
const meshDir = join(REF, 'share', 'meshes')
const namedMeshes = new Set(readdirSync(meshDir).filter(file => file.endsWith('.obj')).map(file => `share/meshes/${file}`))
function refMeshPath (rel) { return join(REF, rel) }
console.log(`BUILTIN MESH INPUTS: ${namedMeshes.size}`)

// ------------------------------------------------------------------- corpus

function mulberry32 (seed) {
  return () => {
    seed |= 0; seed = seed + 0x6D2B79F5 | 0
    let t = Math.imul(seed ^ seed >>> 15, 1 | seed)
    t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t
    return ((t ^ t >>> 14) >>> 0) / 4294967296
  }
}

const cases = [] // { name, mode: 'string'|'file', bytes: Buffer }
const text = (name, s, mode = 'string') => cases.push({ name, mode, bytes: Buffer.from(s, 'utf8') })
const raw = (name, bytes) => cases.push({ name, mode: 'file', bytes: Buffer.from(bytes) })

for (const rel of [...namedMeshes].sort()) {
  const bytes = readFileSync(refMeshPath(rel))
  cases.push({ name: `builtin ${rel}`, mode: 'string', bytes })
  cases.push({ name: `builtin ${rel}`, mode: 'file', bytes })
}

text('empty', '')
text('whitespace only', ' \t\r\n\n　\n')
text('comments only', '# a\n   # b\n#\n')
text('no trailing newline', 'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3')
text('quad v/vt/vn', 'v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\nf 1/1/1 2/2/1 3/3/1 4/4/1\n')
text('n-gon', 'v 0 0 0\nv 1 0 0\nv 2 1 0\nv 1 2 0\nv 0 1 0\nv -1 1 0\nv -1 0 0\nf 1 2 3 4 5 6 7\n')
text('computed normals shared', 'v 0 0 0\nv 0 1 0\nv 1 0 0\nv 0 0 1\nf 1 2 3\nf 1 4 2\n')
text('key rounding', 'v 0 0 0\nv 1 0 0\nv 0 1 0\nv -0.00001 0.00004 0\nv 0.00005 0 1\nv 0.000049999 2 0\nv 0.00015 0 0\nf 1 2 3\nf 4 5 6\nf 7 5 6\n')
text('degenerate triangles', 'v 0 0 0\nv 0 0 0\nv 0 0 0\nv 1 1 1\nf 1 2 3\nf 1 1 4\nf 4 4 4\n')
text('unreferenced vn', 'v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 1 0 0\nf 1 2 3\n')
text('placeholders', 'v 1 2 3\nv 4 5 6\nvn 0 1 0\nvt 0.5 0.5\nf 1/9/1 2/1/7 9 0 -1\n')
text('index forms', 'v 1 0 0\nv 2 0 0\nv 3 0 0\nvt 0.5 0.25\nvn 0 0 1\nf 1.9/1 +2//x 3/abc\nf 1/ /2 3//\nf 1/1/1/1 2/1/1/9 3\nf 99999999999999999999 1 2\n')
text('numbers', 'v 1e400 -0 abc\nv .5 5. -.5e1\nv 0x10 1_000 Infinityx\nv -Infinity 1e 2e+\nv\nv 5e-324 1.7976931348623159e308 -1e-400\nv 1.7976931348623157e308 2.4703282292062328e-324 3.4028235677973366e38\nv 3.4028235677973362e38 1.401298464324817e-45 7.006492321624085e-46\nv 0.1000000000000000055511151231257827021181583404541015625 9007199254740993 -.e5\nf 1 2 3\nf 4 5 6\nf 7 8 9\n')
text('infinite positions', 'v Infinity 0 0\nv 0 -Infinity 0\nv 0 0 1e999\nv 1 1 1\nf 1 2 3\nf 1 2 4\nf 2 3 4\n')
text('unicode whitespace', '﻿ v 1\t2　3\r\nv 4\u000B5\u000C6 v 7 8 9\rv 1 1 1\n   # v 9 9 9\n\nv 0 1 0 \nv​1 2 3\nv᠎1 2 3\nv\u00851 2 3\nf 1 2 3\n')
text('crlf', 'v 0 0 0\r\nv 1 0 0\r\nv 0 1 0\r\nf 1 2 3\r\n')
text('cr only', 'v 0 0 0\rv 1 0 0\rv 0 1 0\rf 1 2 3\r')
text('unknown commands', 'o obj\ng grp\ns 1\nusemtl m\nmtllib m.mtl\nV 1 1 1\nvp 1 1\nl 1 2\nv 1 1 1\nv 2 2 2\nv 3 3 4\nf 1 2\nf 1 2 3\n')
text('bom string', '﻿v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n')
text('bom file', '﻿v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n', 'file')
{
  let big = 'v 0 0 0\nv 1 0 0\nv 0 1 0\n'
  for (let i = 0; i < 22000; i++) big += 'f 1 2 3\n'
  text('truncation past 65536 vertices', big)
}
raw('invalid byte', Buffer.concat([Buffer.from('v 1 2 3\n'), Buffer.from([0xff]), Buffer.from(' v 4 5 6\nv 7 8 9\nf 1 2 3\n')]))
raw('truncated lead before LF', Buffer.concat([Buffer.from('v 1 2 3'), Buffer.from([0xe2]), Buffer.from('\nv 4 5 6\nv 7 8 9\nf 1 2 3\n')]))
raw('truncated lead before space', Buffer.concat([Buffer.from('v 1'), Buffer.from([0xe2, 0x80]), Buffer.from(' 2 3\nv 4 5 6\nv 7 8 9\nf 1 2 3\n')]))
raw('overlong digit', Buffer.concat([Buffer.from('v '), Buffer.from([0xc0, 0xb1]), Buffer.from(' 2 3\nv 4 5 6\nv 7 8 9\nf 1 2 3\n')]))
raw('encoded surrogate', Buffer.concat([Buffer.from('v 1 '), Buffer.from([0xed, 0xa0, 0x80]), Buffer.from(' 3\nv 4 5 6\nv 7 8 9\nf 1 2 3\n')]))
raw('lone continuation', Buffer.concat([Buffer.from('v 1'), Buffer.from([0x80]), Buffer.from(' 2 3\nv 4 5 6\nv 7 8 9\nf 1 2 3\n')]))
raw('encoded line separator', Buffer.concat([Buffer.from('v 1'), Buffer.from([0xe2, 0x80, 0xa8]), Buffer.from('2 3\nv 4 5 6\nv 7 8 9\nf 1 2 3\n')]))
raw('four-byte character', Buffer.from('v 1\u{1F600} 2 3\nv 4 5 6\nv 7 8 9\nf 1 2 3\n', 'utf8'))

// Seeded random OBJ-like texts.
{
  const rand = mulberry32(0x0b1e)
  const pick = (list) => list[Math.floor(rand() * list.length)]
  const fixedNumbers = ['0', '1', '-1', '0.5', '.5', '5.', '-.5e1', '1e3', '1E-3', '1e', '2e+', '1e400', '-1e400',
    '1e-400', '-0', '+0', '5e-324', 'Infinity', '-Infinity', '+Infinity', 'Infinityx', 'infinity', 'NaN', '0x10',
    '1_000', 'abc', '1.2.3', '--1', '+-1', '1e+2e3', '.', '-.', '.e5', '00012', '3.14159265358979323846264338327950288',
    '1.7976931348623159e308', '9007199254740993', '1e-7', '123456.789e-2', '0.000049999', '0.00005', '-0.00005']
  const digits = (n) => { let s = ''; for (let i = 0; i < n; i++) s += String(Math.floor(rand() * 10)); return s }
  const randomNumber = () => {
    if (rand() < 0.35) return pick(fixedNumbers)
    let s = pick(['', '', '-', '+'])
    s += digits(Math.floor(rand() * 6))
    if (rand() < 0.7) s += '.' + digits(Math.floor(rand() * 8))
    if (rand() < 0.25) s += pick(['e', 'E']) + pick(['', '-', '+']) + digits(Math.floor(rand() * 3))
    return s
  }
  const separators = [' ', ' ', ' ', '  ', '\t', ' ', '　', ' ', '\v', '\f', ' \r ', '​', '᠎', '\u0085', ' ', '﻿']
  const lineEnds = ['\n', '\n', '\n', '\r\n', '\n\n', '\r']
  const ref = (count) => {
    const i = () => String(Math.floor(rand() * (count + 5)) - 2)
    return pick([
      () => i(), () => `${i()}/${i()}`, () => `${i()}//${i()}`, () => `${i()}/${i()}/${i()}`, () => `${i()}/`,
      () => `/${i()}`, () => `${i()}/${i()}/${i()}/${i()}`, () => 'x', () => `${i()}.5`, () => `+${i()}`,
      () => '99999999999999999999', () => `${i()}//`, () => `${i()}/x/${i()}`
    ])()
  }
  for (let c = 0; c < FUZZ_CASES; c++) {
    const lines = []
    const lineCount = 1 + Math.floor(rand() * 40)
    const sep = () => pick(separators)
    for (let l = 0; l < lineCount; l++) {
      const lead = rand() < 0.1 ? pick(separators) : ''
      const kind = rand()
      let line
      if (kind < 0.3) line = ['v', randomNumber(), randomNumber(), randomNumber()].concat(rand() < 0.2 ? [randomNumber()] : []).slice(0, 2 + Math.floor(rand() * 4)).join(sep())
      else if (kind < 0.4) line = ['vn', randomNumber(), randomNumber(), randomNumber()].join(sep())
      else if (kind < 0.5) line = ['vt', randomNumber(), randomNumber()].join(sep())
      else if (kind < 0.85) {
        const refs = []
        const n = Math.floor(rand() * 8)
        for (let k = 0; k < n; k++) refs.push(ref(lineCount))
        line = ['f', ...refs].join(sep())
      } else line = pick(['# comment', '#', 'o thing', 'g', 's off', 'usemtl mat', 'V 1 2 3', 'vp 1 2', 'l 1 2', '', 'f', 'vn', 'v'])
      lines.push(lead + line + (rand() < 0.1 ? pick(separators) : ''))
    }
    let body = ''
    for (const line of lines) body += line + pick(lineEnds)
    if (rand() < 0.1) body = '﻿' + body
    text(`fuzz ${c}`, body, rand() < 0.3 ? 'file' : 'string')
  }
}

// ------------------------------------------------------------ reference side

// Float32 bit patterns with every NaN folded to 7fc00000 (both sides).
const canonicalBits = (arr) => {
  const f32 = arr instanceof Float32Array ? arr : new Float32Array(arr)
  const bits = new Uint32Array(f32.length)
  bits.set(new Uint32Array(f32.buffer, f32.byteOffset, f32.length))
  for (let i = 0; i < bits.length; i++) {
    if ((bits[i] & 0x7f800000) === 0x7f800000 && (bits[i] & 0x007fffff) !== 0) bits[i] = 0x7fc00000
  }
  return bits
}
const hexArray = (arr) => {
  const parts = []
  for (const b of canonicalBits(arr)) parts.push(b.toString(16).padStart(8, '0'))
  return parts.join('')
}
if (endianness() !== 'LE') { console.error('check_obj_parser.mjs digests assume a little-endian host'); process.exit(3) }
const digest = (arr) => createHash('sha256').update(new Uint8Array(canonicalBits(arr).buffer)).digest('hex')

async function referenceDump (entry) {
  const objText = entry.mode === 'file'
    ? await new Response(entry.bytes).text()
    : new TextDecoder('utf-8', { ignoreBOM: true }).decode(entry.bytes)
  const mesh = parseOBJ(objText)
  const warn = console.warn
  console.warn = () => {} // truncation warning
  try {
    const large = packMeshDataForTextures(mesh.positions, mesh.normals, mesh.uvs, 256, 256)
    const small = packMeshDataForTextures(mesh.positions, mesh.normals, mesh.uvs, 4, 4)
    return {
      vertexCount: mesh.vertexCount,
      positions: hexArray(mesh.positions),
      normals: hexArray(mesh.normals),
      uvs: hexArray(mesh.uvs),
      packed256: {
        vertexCount: large.vertexCount,
        positionData: digest(large.positionData),
        normalData: digest(large.normalData),
        uvData: digest(large.uvData)
      },
      packed4: {
        vertexCount: small.vertexCount,
        positionData: hexArray(small.positionData),
        normalData: hexArray(small.normalData),
        uvData: hexArray(small.uvData)
      }
    }
  } finally {
    console.warn = warn
  }
}

// ------------------------------------------------------------------ compare

function firstHexDifference (a, b) {
  const n = Math.min(a.length, b.length)
  for (let i = 0; i < n; i += 8) {
    if (a.slice(i, i + 8) !== b.slice(i, i + 8)) return `float ${i / 8}: reference ${a.slice(i, i + 8)} candidate ${b.slice(i, i + 8)}`
  }
  return `length reference ${a.length / 8} candidate ${b.length / 8}`
}

const dir = mkdtempSync(join(tmpdir(), 'nm-obj-'))
try {
  const manifest = cases.map((c, i) => {
    const path = join(dir, `case${i}.obj`)
    writeFileSync(path, c.bytes)
    return { path, mode: c.mode }
  })
  const manifestPath = join(dir, 'manifest.json')
  writeFileSync(manifestPath, JSON.stringify(manifest))
  const candidate = JSON.parse(execFileSync(DUMP, [manifestPath], { encoding: 'utf8', maxBuffer: 1 << 30 }))
  if (candidate.length !== cases.length) {
    fail(`candidate returned ${candidate.length} results for ${cases.length} inputs`)
  }

  let passed = 0
  for (let i = 0; i < cases.length; i++) {
    const c = cases[i]
    const got = candidate[i] || {}
    if (got.error) { fail(`${c.name}: candidate error ${got.error}`); continue }
    const want = await referenceDump(c)
    const problems = []
    if (got.vertexCount !== want.vertexCount) problems.push(`vertexCount reference ${want.vertexCount} candidate ${got.vertexCount}`)
    for (const key of ['positions', 'normals', 'uvs']) {
      if (got[key] !== want[key]) problems.push(`${key} ${firstHexDifference(want[key], got[key] || '')}`)
    }
    for (const pack of ['packed256', 'packed4']) {
      for (const key of ['vertexCount', 'positionData', 'normalData', 'uvData']) {
        const a = want[pack][key]
        const b = got[pack]?.[key]
        if (a !== b) {
          problems.push(`${pack}.${key} ${pack === 'packed4' && key !== 'vertexCount' ? firstHexDifference(a, b || '') : `reference ${a} candidate ${b}`}`)
        }
      }
    }
    if (problems.length) fail(`${c.name} (${c.mode}): ${problems.join('; ')}`)
    else passed++
  }
  console.log(`OBJ PARSER: ${passed}/${cases.length} inputs identical`)
} finally {
  rmSync(dir, { recursive: true, force: true })
}

if (failures) {
  console.log(`${failures} FAILURE(S)`)
  process.exit(1)
}
