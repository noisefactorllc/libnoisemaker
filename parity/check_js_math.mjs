#!/usr/bin/env node
// check_js_math.mjs — tools/js-math.mjs, the Math.sin and Math.cos the
// pipeline oracle evaluates the reference with, against the port's js::sin
// and js::cos (`nm-dump js-math`), bit for bit: zeros, NaN and infinities,
// multiples of pi / 4 and the doubles on either side, arguments whose
// quadrant estimate is a half-integer, the double nearest a multiple of
// pi / 2, and seeded random arguments of every exponent field, subnormals
// and arguments beyond 2^20 included.
//
//   NM_DSL=build/nm-dump node parity/check_js_math.mjs
import { execFileSync } from 'node:child_process'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import * as jsMath from '../tools/js-math.mjs'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..')
const NM_DSL = process.env.NM_DSL || join(ROOT, 'build', 'nm-dump')

const view = new DataView(new ArrayBuffer(8))
const bitsOf = x => { view.setFloat64(0, x); return view.getBigUint64(0).toString(16).padStart(16, '0') }
const fromBits = b => { view.setBigUint64(0, b); return view.getFloat64(0) }

// xorshift64*, seeded: the same arguments on every run.
let state = 0x9e3779b97f4a7c15n
const next = () => {
  state ^= state >> 12n; state ^= (state << 25n) & 0xffffffffffffffffn; state ^= state >> 27n
  return (state * 0x2545f4914f6cdd1dn) & 0xffffffffffffffffn
}

const inputs = [0, -0, NaN, Infinity, -Infinity, 5e-324, -5e-324, 2.2250738585072014e-308, 1e-300, 1e-160, 1e-8, 0.5, 1,
  Math.PI / 4, Math.PI / 2, Math.PI, 2 * Math.PI, 1048575.9999999999, -1048575.9999999999, 1048576, -1048576, 1e8, 1e22,
  -1e22, 1.7976931348623157e308, -1.7976931348623157e308, 6381956970095103 * 2 ** 797, -6381956970095103 * 2 ** 797]
// The next double away from zero, and toward it.
const neighbour = x => { view.setFloat64(0, x); view.setBigUint64(0, view.getBigUint64(0) + 1n); return view.getFloat64(0) }
const inward = x => { view.setFloat64(0, x); view.setBigUint64(0, view.getBigUint64(0) - 1n); return view.getFloat64(0) }
for (let k = -64; k <= 64; k++) {
  const x = k * Math.PI / 4
  inputs.push(x, neighbour(x))
  if (x !== 0) inputs.push(inward(x))
}
for (let k = 1; k <= 1e6; k *= 10) {
  for (const x of [k * 1048576 * Math.PI / 2, 2 ** 52 * Math.PI, 2 ** 900 * Math.PI]) inputs.push(x, neighbour(x), inward(x), -x)
}
// Arguments whose quadrant estimate x * 2 / pi is exactly a half-integer,
// which the reduction rounds to even.
for (let h = 0.5; h < 200000; h += 1) {
  for (const x of [h / 6.36619772367581382433e-01, neighbour(h / 6.36619772367581382433e-01)]) {
    if (x * 6.36619772367581382433e-01 === h) inputs.push(x, -x)
  }
}
// Every exponent field from 0 (subnormals) to 2046, both signs, random
// mantissas.
for (let i = 0; i < 200000; i++) {
  const r = next()
  const exponent = Number(r % 2047n)
  const mantissa = next() & 0xfffffffffffffn
  const sign = (r >> 63n) << 63n
  inputs.push(fromBits(sign | (BigInt(exponent) << 52n) | mantissa))
}
// Arguments an automation reaches: t * 2 * pi and angles below 400 rad.
for (let i = 0; i < 100000; i++) inputs.push(Number(next() >> 11n) / 2 ** 53 * 400 - 200)

const output = execFileSync(NM_DSL, ['js-math'], { input: inputs.map(bitsOf).join('\n') + '\n', encoding: 'utf8', maxBuffer: 1 << 28 })
const lines = output.trim().split('\n')
if (lines.length !== inputs.length) { console.error(`JS_MATH: ${lines.length} results for ${inputs.length} arguments`); process.exit(1) }
let matched = 0
let shown = 0
inputs.forEach((x, i) => {
  const expected = `${bitsOf(jsMath.sin(x))} ${bitsOf(jsMath.cos(x))}`
  if (lines[i] === expected) { matched++; return }
  if (shown++ < 10) console.error(`  ${x} (${bitsOf(x)}): js-math.mjs ${expected}, js::sin/cos ${lines[i]}`)
})
console.log(`JS_MATH: ${matched}/${inputs.length} sin and cos results identical`)
process.exit(matched === inputs.length ? 0 : 1)
