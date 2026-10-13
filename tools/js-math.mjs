// js-math.mjs — the correctly rounded Math.sin and Math.cos of
// src/core/value/js_math.cpp, step for step in JavaScript: the same
// double-double argument reduction and Taylor series, with Dekker's exact
// product where the C++ uses fma. parity/check_js_math.mjs checks that the two
// agree bit for bit.
//
// The pipeline oracle evaluates the reference with these, because the last
// bit of V8's Math.sin and Math.cos depends on the platform: CI run
// 38336626478 found an automation value of 0.849637851995162 from Node 26.5.1
// on Linux x64 where Node 26.0.0 and 26.10.0 on macOS arm64, and the port,
// give 0.8496378519951611. The port follows the correctly rounded results
// (js_math.h), so the gate checks everything but that bit.

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

// [r, quadrant] with x = k * pi / 2 + r, or null at or beyond 2^20.
function reduceQuadrant (x) {
  if (!(Math.abs(x) < 1048576)) return null
  const k = nearbyint(x * 6.36619772367581382433e-01)
  let acc = [x, 0]
  acc = ddAdd(acc, [-k * 1.57079632673412561417e+00, 0])
  acc = ddAdd(acc, [-k * 6.07710050630396597660e-11, 0])
  acc = ddAdd(acc, [-k * 2.02226624871116645580e-21, 0])
  const p = twoProd(k, 8.47842766036889956997e-32)
  acc = ddAdd(acc, [-p[0], -p[1]])
  return [acc, ((k % 4) + 4) % 4]
}

// Arguments at or beyond 2^20 fall back to the platform's functions, as the
// C++ does.
export function makeJsMath (platformSin, platformCos) {
  return {
    sin (x) {
      x = Number(x)
      if (x === 0) return x
      if (!Number.isFinite(x)) return NaN
      const reduced = reduceQuadrant(x)
      if (!reduced) return platformSin(x)
      const [r, quadrant] = reduced
      switch (quadrant) {
        case 0: return sinSeries(r)[0]
        case 1: return cosSeries(r)[0]
        case 2: return -sinSeries(r)[0]
        default: return -cosSeries(r)[0]
      }
    },
    cos (x) {
      x = Number(x)
      if (!Number.isFinite(x)) return NaN
      if (x === 0) return 1
      const reduced = reduceQuadrant(x)
      if (!reduced) return platformCos(x)
      const [r, quadrant] = reduced
      switch (quadrant) {
        case 0: return cosSeries(r)[0]
        case 1: return -sinSeries(r)[0]
        case 2: return -cosSeries(r)[0]
        default: return sinSeries(r)[0]
      }
    }
  }
}
