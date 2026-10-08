#!/usr/bin/env node
import { existsSync, mkdirSync, readFileSync, readdirSync } from 'node:fs'
import { join, relative, resolve, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { execFileSync } from 'node:child_process'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..')
const out = resolve(process.argv[2] || join(root, 'build/catalog'))
const reference = execFileSync(join(root, 'scripts/reference'), { encoding: 'utf8' }).trim()
const siblingQt = join(root, '../noisemaker-for-qt')
const qtRoot = resolve(process.env.NM_QT_ROOT ||
  (existsSync(join(siblingQt, '.git')) ? siblingQt : join(root, '.cache/qt-reference')))
if (!existsSync(join(qtRoot, '.git'))) {
  mkdirSync(dirname(qtRoot), { recursive: true })
  execFileSync('git', ['clone', '--filter=blob:none', '--no-checkout',
    'https://github.com/noisefactorllc/noisemaker-for-qt', qtRoot], { stdio: 'inherit' })
}
execFileSync('git', ['-C', qtRoot, 'cat-file', '-e', '4058164^{commit}'])
const errors = []
let wgsl = 0
let glsl = 0
let definitions = 0

function files (dir) {
  return readdirSync(dir, { withFileTypes: true }).flatMap(entry => {
    const path = join(dir, entry.name)
    return entry.isDirectory() ? files(path) : [path]
  }).sort()
}
function recordDifference (label, a, b, location = '') {
  if (Object.is(a, b)) return
  if (a && b && typeof a === 'object' && typeof b === 'object') {
    if (Array.isArray(a) !== Array.isArray(b)) {
      errors.push(`${label}${location}: array/object mismatch`)
      return
    }
    if (Array.isArray(a)) {
      if (a.length !== b.length) errors.push(`${label}${location}: array length ${a.length} != ${b.length}`)
      for (let i = 0; i < Math.max(a.length, b.length); i++) recordDifference(label, a[i], b[i], `${location}[${i}]`)
    } else {
      const aKeys = Object.keys(a)
      const bKeys = Object.keys(b)
      if (aKeys.join('\0') !== bKeys.join('\0')) errors.push(`${label}${location}: key order/set ${JSON.stringify(aKeys)} != ${JSON.stringify(bKeys)}`)
      for (const key of new Set([...aKeys, ...bKeys])) recordDifference(label, a[key], b[key], `${location}.${key}`)
    }
    return
  }
  errors.push(`${label}${location}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`)
}

for (const lang of ['wgsl', 'glsl']) {
  const sourceFiles = files(join(reference, 'shaders/effects')).filter(path => {
    const rel = relative(join(reference, 'shaders/effects'), path).replaceAll('\\', '/')
    return rel.includes(`/${lang}/`) && rel.endsWith(`.${lang}`)
  })
  for (const path of sourceFiles) {
    const rel = relative(join(reference, 'shaders/effects'), path).replaceAll('\\', '/')
    const outputRel = rel.replace(`/${lang}/`, '/')
    if (!existsSync(join(out, lang, outputRel))) errors.push(`${lang}/${outputRel}: missing from catalog`)
  }
  for (const path of files(join(out, lang))) {
    const rel = relative(join(out, lang), path)
    const pieces = rel.split('/')
    const program = pieces.pop()
    const source = join(reference, 'shaders/effects', ...pieces, lang, program)
    let expected
    try { expected = readFileSync(source) } catch { errors.push(`${lang}/${rel}: missing source ${source}`); continue }
    if (!readFileSync(path).equals(expected)) errors.push(`${lang}/${rel}: byte difference from pinned source`)
    if (lang === 'wgsl') wgsl++
    else glsl++
  }
}

const hooks = {}
for (const path of files(join(out, 'effects'))) {
  const rel = relative(join(out, 'effects'), path).replaceAll('\\', '/')
  const actual = JSON.parse(readFileSync(path, 'utf8'))
  const key = rel.slice(0, -5)
  if (actual.jsHooks) hooks[key] = actual.jsHooks
  delete actual.jsHooks
  delete actual.uniformLayouts
  delete actual.sourceNamespace
  for (const pass of actual.passes || []) delete pass.viewport
  for (const spec of Object.values(actual.globals || {})) {
    if (spec.ui) {
      delete spec.ui.format
      if (spec.ui.control !== false) delete spec.ui.control
      if (Object.keys(spec.ui).length === 0) delete spec.ui
    }
  }
  const qtPath = `qt/noisemaker/effects/${rel}`
  let expected
  try {
    expected = JSON.parse(execFileSync('git', ['-C', qtRoot, 'show', `4058164:${qtPath}`], { encoding: 'utf8' }))
  } catch {
    errors.push(`effects/${rel}: missing qt definition at 4058164`)
    continue
  }
  delete expected.uniformLayouts
  // The older Qt projection supplied a default format even when the pinned
  // definition omitted one. Preserve that absence for frontend graph parity.
  for (const [id, spec] of Object.entries(expected.textures || {})) {
    if (spec.format === 'rgba16f' && actual.textures?.[id]?.format === undefined) delete spec.format
  }
  recordDifference(`effects/${rel}`, actual, expected)
  definitions++
}
recordDifference('jsHooks', hooks, {
  'filter/fibers': ['asyncInit'],
  'filter/scratches': ['asyncInit'],
  'filter/strayHair': ['asyncInit'],
  'synth/media': ['onInit', 'onUpdate', 'setMediaDimensions']
})
if (definitions !== 210) errors.push(`definitions: ${definitions} != 210`)
for (const error of errors) console.error(error)
console.log(`CATALOG: wgsl ${wgsl}/${wgsl}, glsl ${glsl}/${glsl}, definitions ${definitions}/210, hooks ${Object.keys(hooks).length}/4`)
if (errors.length) process.exitCode = 1
