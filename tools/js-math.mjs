// js-math.mjs — the correctly rounded Math.sin and Math.cos of
// src/core/value/js_math.cpp in JavaScript: the same argument reductions and
// Taylor series in double-double arithmetic. Where the C++ uses fma for the
// error of a product, this uses Dekker's product; the two agree unless the
// product is near the subnormal range, where neither reaches a result's high
// part. parity/check_js_math.mjs checks that the two agree bit for bit.
//
// The pipeline oracle evaluates the reference with these, because the last
// bit of V8's Math.sin and Math.cos is not the same in every V8 build: CI
// run 38336626478 read an automation value of 0.849637851995162 from Node
// 26.5.1 on Linux x64, where Node 26.0.0 and 26.10.0 on macOS arm64, and
// the port, give 0.8496378519951611. The port follows the correctly
// rounded results (js_math.h), so the gate checks everything but that bit;
// tests/core/test_js_number.cpp checks the port's against results computed
// independently.

const SPLIT = 134217729 // 2^27 + 1

function twoSum (a, b) {
  const s = a + b
  const bb = s - a
  return [s, (a - (s - bb)) + (b - bb)]
}

function quickTwoSum (a, b) {
  const s = a + b
  return [s, b - (s - a)]
}

function twoProd (a, b) {
  const p = a * b
  let t = SPLIT * a
  const ah = t - (t - a)
  const al = a - ah
  t = SPLIT * b
  const bh = t - (t - b)
  const bl = b - bh
  return [p, ((ah * bh - p) + ah * bl + al * bh) + al * bl]
}

function ddAdd (a, b) {
  let s = twoSum(a[0], b[0])
  const t = twoSum(a[1], b[1])
  s = quickTwoSum(s[0], s[1] + t[0])
  return quickTwoSum(s[0], s[1] + t[1])
}

function ddMul (a, b) {
  const p = twoProd(a[0], b[0])
  return quickTwoSum(p[0], p[1] + (a[0] * b[1] + a[1] * b[0]))
}

function ddDivD (a, b) {
  const q1 = a[0] / b
  const p = twoProd(q1, b)
  const r = ((a[0] - p[0]) - p[1] + a[1]) / b
  return quickTwoSum(q1, r)
}

function sinSeries (r) {
  const r2 = ddMul(r, r)
  let term = r
  let sum = r
  for (let n = 3; n <= 33; n += 2) {
    term = ddDivD(ddMul(term, r2), -((n - 1) * n))
    sum = ddAdd(sum, term)
  }
  return sum
}

function cosSeries (r) {
  const r2 = ddMul(r, r)
  let term = [1, 0]
  let sum = [1, 0]
  for (let n = 2; n <= 32; n += 2) {
    term = ddDivD(ddMul(term, r2), -((n - 1) * n))
    sum = ddAdd(sum, term)
  }
  return sum
}

// Round half to even, as std::nearbyint does.
function nearbyint (v) {
  let k = Math.round(v)
  if (k - v === 0.5 && k % 2 !== 0) k -= 1
  return k
}

// [r, quadrant] with x = k * pi / 2 + r for |x| < 2^20.
function reduceQuadrant (x) {
  const k = nearbyint(x * 6.36619772367581382433e-01)
  let acc = [x, 0]
  acc = ddAdd(acc, [-k * 1.57079632673412561417e+00, 0])
  acc = ddAdd(acc, [-k * 6.07710050630396597660e-11, 0])
  acc = ddAdd(acc, [-k * 2.02226624871116645580e-21, 0])
  const p = twoProd(k, 8.47842766036889956997e-32)
  acc = ddAdd(acc, [-p[0], -p[1]])
  return [acc, ((k % 4) + 4) % 4]
}

// 2 / pi in 32-bit words, most significant first.
const TWO_OVER_PI = [
  0xa2f9836e, 0x4e441529, 0xfc2757d1, 0xf534ddc0, 0xdb629599, 0x3c439041, 0xfe5163ab, 0xdebbc561,
  0xb7246e3a, 0x424dd2e0, 0x06492eea, 0x09d1921c, 0xfe1deb1c, 0xb129a73e, 0xe88235f5, 0x2ebb4484,
  0xe99c7026, 0xb45f7e41, 0x3991d639, 0x835339f4, 0x9c845f8b, 0xbdf9283b, 0x1ff897ff, 0xde05980f,
  0xef2f118b, 0x5a0a6d1f, 0x6d367ecf, 0x27cb09b7, 0x4f463f66, 0x9e5fea2d, 0x7527bac7, 0xebe5f17b,
  0x3d0739f7, 0x8a5292ea, 0x6bfb5fb1, 0x1f8d5d08, 0x56033046, 0xfc7b6bab, 0xf0cfbc20
].map(BigInt)

const view = new DataView(new ArrayBuffer(8))
// 2^n for a normal exponent n, built from its bits.
function pow2 (n) {
  view.setBigUint64(0, BigInt(n + 1023) << 52n)
  return view.getFloat64(0)
}

// Bits [top - 52, top] of a non-negative BigInt; bits below 0 are zero.
function bitsBelow (value, top) {
  const shifted = top >= 52 ? value >> BigInt(top - 52) : value << BigInt(52 - top)
  return Number(shifted & 0x1fffffffffffffn)
}

// [r, quadrant] with x = k * pi / 2 + r for finite x >= 2^20 (Payne and
// Hanek), in the C++'s steps with exact BigInt products.
function reduceLarge (x) {
  view.setFloat64(0, x)
  const raw = view.getBigUint64(0)
  const m = (raw & 0xfffffffffffffn) | 0x10000000000000n
  const e = Number(raw >> 52n) - 1075 // x = m * 2^e
  const wStart = e >= 2 ? Math.floor((e - 2) / 32) : 0
  const WORDS = 9
  let w = 0n
  for (let i = 0; i < WORDS; i++) w = (w << 32n) | TWO_OVER_PI[wStart + i]
  const product = m * w
  const fractionBits = 32 * (wStart + WORDS) - e
  let quadrant = Number((product >> BigInt(fractionBits)) & 3n)
  let fraction = product & ((1n << BigInt(fractionBits)) - 1n)
  const negative = ((fraction >> BigInt(fractionBits - 1)) & 1n) === 1n
  if (negative) {
    fraction = (1n << BigInt(fractionBits)) - fraction
    quadrant = (quadrant + 1) % 4
  }
  if (fraction === 0n) return [[0, 0], quadrant]
  const top = fraction.toString(2).length - 1
  const hi = bitsBelow(fraction, top) * pow2(top - 52 - fractionBits)
  const lo = bitsBelow(fraction, top - 53) * pow2(top - 105 - fractionBits)
  let f = quickTwoSum(hi, lo)
  if (negative) f = [-f[0], -f[1]]
  return [ddMul(f, [1.57079632679489655800e+00, 6.12323399573676588613e-17]), quadrant]
}

function reduce (x) {
  if (Math.abs(x) < 1048576) return reduceQuadrant(x)
  // Reduce the magnitude: sin is odd and cos even.
  const [r, quadrant] = reduceLarge(Math.abs(x))
  return x < 0 ? [[-r[0], -r[1]], (4 - quadrant) % 4] : [r, quadrant]
}

export function sin (x) {
  x = Number(x)
  if (x === 0) return x
  if (!Number.isFinite(x)) return NaN
  const [r, quadrant] = reduce(x)
  switch (quadrant) {
    case 0: return sinSeries(r)[0]
    case 1: return cosSeries(r)[0]
    case 2: return -sinSeries(r)[0]
    default: return -cosSeries(r)[0]
  }
}

export function cos (x) {
  x = Number(x)
  if (!Number.isFinite(x)) return NaN
  if (x === 0) return 1
  const [r, quadrant] = reduce(x)
  switch (quadrant) {
    case 0: return cosSeries(r)[0]
    case 1: return -sinSeries(r)[0]
    case 2: return -cosSeries(r)[0]
    default: return sinSeries(r)[0]
  }
}
