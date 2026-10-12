#!/usr/bin/env node
import { existsSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, relative, resolve, dirname } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { execFileSync } from 'node:child_process'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..')
const out = resolve(process.argv[2] || join(root, 'build/catalog'))
const reference = execFileSync(join(root, 'scripts/reference'), { encoding: 'utf8' }).trim()
// Check 2 compares each catalog definition, projected from the published bundles, field for
// field with the same projection run on the reference's own definition.js files at the pin.
const projected = mkdtempSync(join(tmpdir(), 'nm-catalog-definitions-'))
process.on('exit', () => rmSync(projected, { recursive: true, force: true }))
try {
  execFileSync(process.execPath, [join(root, 'tools/catalog/convert-definitions.mjs')], {
    env: { ...process.env, NM_REFERENCE_ROOT: reference, NM_OUT_DIR: projected },
    stdio: ['ignore', 'ignore', 'pipe']
  })
} catch (error) {
  process.stderr.write(error.stderr ?? '')
  throw error
}
const errors = []
let definitions = 0

// The effects the reference declares at the pin: every <namespace>/<name>/definition.js,
// found independently of the converter's own enumeration and the catalog builder's.
function referenceEffects () {
  const effectsRoot = join(reference, 'shaders/effects')
  const live = new Set()
  for (const ns of readdirSync(effectsRoot, { withFileTypes: true })) {
    if (!ns.isDirectory()) continue
    for (const effect of readdirSync(join(effectsRoot, ns.name), { withFileTypes: true })) {
      if (effect.isDirectory() && existsSync(join(effectsRoot, ns.name, effect.name, 'definition.js'))) {
        live.add(`${ns.name}/${effect.name}`)
      }
    }
  }
  return live
}

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

const shaders = {}
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
  let matched = 0
  for (const path of files(join(out, lang))) {
    const rel = relative(join(out, lang), path).replaceAll('\\', '/')
    const pieces = rel.split('/')
    const program = pieces.pop()
    const source = join(reference, 'shaders/effects', ...pieces, lang, program)
    let expected
    try { expected = readFileSync(source) } catch { errors.push(`${lang}/${rel}: missing source ${source}`); continue }
    if (readFileSync(path).equals(expected)) matched++
    else errors.push(`${lang}/${rel}: byte difference from pinned source`)
  }
  shaders[lang] = `${matched}/${sourceFiles.length}`
}

const hooks = {}
const catalogEffects = new Set()
for (const path of files(join(out, 'effects'))) {
  const rel = relative(join(out, 'effects'), path).replaceAll('\\', '/')
  const actual = JSON.parse(readFileSync(path, 'utf8'))
  const key = rel.slice(0, -5)
  if (actual.jsHooks) hooks[key] = actual.jsHooks
  catalogEffects.add(key)
  let expected
  try {
    expected = JSON.parse(readFileSync(join(projected, rel), 'utf8'))
  } catch {
    errors.push(`effects/${rel}: missing from the projection of the pinned definitions`)
    continue
  }
  const before = errors.length
  recordDifference(`effects/${rel}`, actual, expected)
  if (errors.length === before) definitions++
}
const expectedHooks = {
  'filter/fibers': ['asyncInit'],
  'filter/scratches': ['asyncInit'],
  'filter/strayHair': ['asyncInit'],
  'synth/media': ['onInit', 'onUpdate', 'setMediaDimensions']
}
recordDifference('jsHooks', hooks, expectedHooks)
const hooksMatched = Object.keys(expectedHooks)
  .filter(key => JSON.stringify(hooks[key]) === JSON.stringify(expectedHooks[key])).length
const live = referenceEffects()
const projectedEffects = new Set(files(projected).map(path => relative(projected, path).replaceAll('\\', '/').slice(0, -5)))
for (const key of live) {
  if (!catalogEffects.has(key)) errors.push(`effects/${key}: declared by the reference, missing from the catalog`)
  if (!projectedEffects.has(key)) errors.push(`effects/${key}: declared by the reference, missing from the projection`)
}
for (const key of catalogEffects) if (!live.has(key)) errors.push(`effects/${key}: in the catalog, not declared by the reference`)
if (live.size === 0) errors.push('definitions: the reference declares no effects')
// The runtime's own shaders, extracted again from the reference checkout.
const runtimeOut = mkdtempSync(join(tmpdir(), 'nm-catalog-runtime-'))
process.on('exit', () => rmSync(runtimeOut, { recursive: true, force: true }))
execFileSync(process.execPath, [join(root, 'tools/catalog/extract-runtime-shaders.mjs'),
  '--src', join(reference, 'shaders/src'), '--out', runtimeOut, '--sites', join(runtimeOut, 'sites.json')],
{ stdio: ['ignore', 'ignore', 'inherit'] })
// Every createShaderModule call in the runtime's source text either compiles a
// program's own source (`code: source` or `code: vertexSource` inside
// WebGPUBackend's compileComputeProgram and compileRenderProgram) or is one the
// extractor captured, and the name has no other use. The runtime's modules are
// parsed with the acorn that the reference itself bundles (shade-mcp's
// formats module, at the pin), loaded from a copy that exports its parser, so
// comments, strings and templates are never mistaken for calls.
const capturedSites = new Set(Object.values(JSON.parse(readFileSync(join(runtimeOut, 'sites.json'), 'utf8'))))
const acornBundle = join(reference, 'vendor/shade-mcp/formats/index.js')
const acornCopy = join(runtimeOut, 'acorn.mjs')
writeFileSync(acornCopy, readFileSync(acornBundle, 'utf8') + '\nexport { Parser as __acornParser };\n')
const { __acornParser: AcornParser } = await import(pathToFileURL(acornCopy).href)
const sourceSites = []
const programSites = []
for (const path of files(join(reference, 'shaders/src')).filter(path => path.endsWith('.js'))) {
  const rel = relative(join(reference, 'shaders/src'), path).replaceAll('\\', '/')
  let ast
  try {
    ast = AcornParser.parse(readFileSync(path, 'utf8'), { ecmaVersion: 'latest', sourceType: 'module', locations: true })
  } catch (error) {
    errors.push(`runtime: ${rel} does not parse: ${error.message}`)
    continue
  }
  const calls = new Set()
  const visit = (node, method) => {
    if (!node || typeof node.type !== 'string') return
    if (node.type === 'MethodDefinition' && node.key?.type === 'Identifier') method = node.key.name
    if (node.type === 'CallExpression' && node.callee.type === 'MemberExpression' && !node.callee.computed &&
        node.callee.property.name === 'createShaderModule') {
      calls.add(node.callee.property)
      const site = `${rel}:${node.callee.property.loc.start.line}`
      const [argument] = node.arguments
      const code = argument?.type === 'ObjectExpression' && argument.properties.length === 1 ? argument.properties[0] : null
      if (rel === 'runtime/backends/webgpu.js' && ['compileComputeProgram', 'compileRenderProgram'].includes(method) &&
          code?.type === 'Property' && code.key.name === 'code' && code.value.type === 'Identifier' &&
          ['source', 'vertexSource'].includes(code.value.name)) programSites.push(site)
      else sourceSites.push(site)
    }
    if ((node.type === 'Identifier' && node.name === 'createShaderModule') ||
        (node.type === 'Literal' && node.value === 'createShaderModule')) {
      if (!calls.has(node)) errors.push(`runtime: ${rel}:${node.loc.start.line} uses createShaderModule other than by calling it`)
    }
    for (const key of Object.keys(node)) {
      if (key === 'loc') continue
      const child = node[key]
      if (Array.isArray(child)) child.forEach(item => visit(item, method))
      else if (child && typeof child === 'object') visit(child, method)
    }
  }
  visit(ast, null)
}
if (programSites.length !== 3) errors.push(`runtime: expected 3 compileProgram shader sites, found ${programSites.length} (${programSites.join(', ')})`)
for (const site of sourceSites) if (!capturedSites.has(site)) errors.push(`runtime: shader module at ${site} is not extracted`)
for (const site of capturedSites) if (!sourceSites.includes(site)) errors.push(`runtime: extracted site ${site} is not a createShaderModule call`)
const runtimeExpected = JSON.parse(readFileSync(join(runtimeOut, 'runtime/shaders.json'), 'utf8'))
const runtimePath = join(out, 'runtime/shaders.json')
const runtimeActual = existsSync(runtimePath) ? JSON.parse(readFileSync(runtimePath, 'utf8')) : null
const runtimeNames = Object.keys(runtimeExpected)
const runtimeMatched = runtimeNames.filter(name => runtimeActual?.[name] === runtimeExpected[name]).length
if (runtimeMatched !== runtimeNames.length || Object.keys(runtimeActual ?? {}).length !== runtimeNames.length) {
  errors.push(`runtime/shaders.json: ${runtimeMatched}/${runtimeNames.length} entries equal the reference's`)
}
for (const error of errors) console.error(error)
console.log(`CATALOG: wgsl ${shaders.wgsl}, glsl ${shaders.glsl}, definitions ${definitions}/${live.size}, hooks ${hooksMatched}/${Object.keys(expectedHooks).length}, runtime ${runtimeMatched}/${runtimeNames.length} (${capturedSites.size}/${sourceSites.length} shader sites)`)
if (errors.length) process.exitCode = 1
