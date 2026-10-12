#!/usr/bin/env node
import { existsSync, mkdtempSync, readFileSync, readdirSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, relative, resolve, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
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
// extractor captured. An acorn-style tokenizer (codeOnly) blanks comments,
// strings, template text and regular expressions first, so a call is found
// however it is spaced or split across lines; any other use of the name is an
// error.
const capturedSites = new Set(Object.values(JSON.parse(readFileSync(join(runtimeOut, 'sites.json'), 'utf8'))))
// Tokenizes JavaScript the way acorn's tokenizer does (tokencontext.js): a
// stack of brace, paren, template and function contexts and the exprAllowed
// flag decide whether a slash starts a regular expression or divides.
// Returns the text with comments, strings, template text and regular
// expressions blanked, so only code remains.
function codeOnly (text) {
  const out = text.split('')
  const blank = (from, to) => { for (let k = from; k < to && k < out.length; k++) if (!'\n\r\u2028\u2029'.includes(out[k])) out[k] = ' ' }
  const ctx = (name, isExpr, token = null) => ({ name, isExpr, token })
  const B_STAT = ctx('b_stat', false); const B_EXPR = ctx('b_expr', true); const B_TMPL = ctx('b_tmpl', true)
  const P_STAT = ctx('p_stat', false); const P_EXPR = ctx('p_expr', true); const Q_TMPL = ctx('q_tmpl', true)
  const F_EXPR = ctx('f_expr', true, 'function'); const F_STAT = ctx('f_stat', false, 'function')
  const stack = [B_STAT]
  const cur = () => stack[stack.length - 1]
  // Keywords acorn marks beforeExpr; after them an expression (and so a regex) may start.
  const beforeExprKeywords = new Set(['case', 'default', 'do', 'else', 'return', 'throw', 'new', 'in', 'instanceof', 'typeof', 'void', 'delete', 'extends'])
  const otherKeywords = new Set(['break', 'catch', 'continue', 'debugger', 'finally', 'for', 'function', 'if', 'switch', 'while', 'with', 'null', 'true', 'false', 'this', 'const', 'class', 'super', 'import', 'export', 'try', 'var'])
  const punctuators = ['>>>=', '...', '===', '!==', '**=', '<<=', '>>=', '>>>', '&&=', '||=', '??=', '=>', '==', '!=', '<=', '>=', '&&', '||', '??', '?.',
    '++', '--', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '**', '<<', '>>']
  const beforeExprPunct = new Set(['[', '{', '(', ',', ';', ':', '?', '=>', '...', '=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '**=', '<<=', '>>=', '>>>=',
    '&&=', '||=', '??=', '==', '!=', '===', '!==', '<', '>', '<=', '>=', '+', '-', '*', '/', '%', '**', '<<', '>>', '>>>', '&', '|', '^', '&&', '||', '??', '!', '~', '?.'])
  let exprAllowed = true
  let prev = { type: 'start', value: '' }
  let lastEnd = 0
  let i = 0
  const lineBreakSince = from => /[\n\r\u2028\u2029]/.test(text.slice(from, i))
  const isIdentStart = c => /[A-Za-z_$\u0080-\uffff\\]/.test(c)
  const isIdentPart = c => /[\w$\u0080-\uffff\\]/.test(c)
  function braceIsBlock (tokenStart) {
    const parent = cur()
    if (parent === F_EXPR || parent === F_STAT) return true
    if (prev.type === ':' && (parent === B_STAT || parent === B_EXPR)) return !parent.isExpr
    if (prev.type === 'return' || (prev.type === 'name' && exprAllowed)) return /[\n\r\u2028\u2029]/.test(text.slice(lastEnd, tokenStart))
    if (['else', ';', 'start', ')', '=>'].includes(prev.type)) return true
    if (prev.type === '{') return parent === B_STAT
    if (['var', 'const', 'name'].includes(prev.type)) return false
    return !exprAllowed
  }
  function update (type, value, tokenStart) {
    if (type === '}' || type === ')') {
      if (stack.length === 1) { exprAllowed = true; return }
      let out = stack.pop()
      if (out === B_STAT && cur().token === 'function') out = stack.pop()
      exprAllowed = !out.isExpr
    } else if (type === '{') {
      stack.push(braceIsBlock(tokenStart) ? B_STAT : B_EXPR); exprAllowed = true
    } else if (type === '${') {
      stack.push(B_TMPL); exprAllowed = true
    } else if (type === '(') {
      stack.push(['if', 'for', 'with', 'while'].includes(prev.type) ? P_STAT : P_EXPR); exprAllowed = true
    } else if (type === '++' || type === '--') {
      // exprAllowed is unchanged
    } else if (type === 'function' || type === 'class') {
      const beforeExpr = beforeExprPunct.has(prev.type) || beforeExprKeywords.has(prev.type)
      if (beforeExpr && prev.type !== 'else' && !(prev.type === ';' && cur() !== P_STAT) &&
          !(prev.type === 'return' && /[\n\r\u2028\u2029]/.test(text.slice(lastEnd, tokenStart))) &&
          !((prev.type === ':' || prev.type === '{') && cur() === B_STAT)) stack.push(F_EXPR)
      else stack.push(F_STAT)
      exprAllowed = false
    } else if (type === '`') {
      if (cur() === Q_TMPL) stack.pop(); else stack.push(Q_TMPL)
      exprAllowed = false
    } else if (type === 'name') {
      exprAllowed = prev.type !== '.' && ((value === 'of' && !exprAllowed) || value === 'yield' || value === 'await')
    } else {
      exprAllowed = beforeExprPunct.has(type) || beforeExprKeywords.has(type)
    }
    prev = { type, value }
  }
  while (i < text.length) {
    if (cur() === Q_TMPL) {
      const from = i
      while (i < text.length && text[i] !== '`' && !(text[i] === '$' && text[i + 1] === '{')) i += text[i] === '\\' ? 2 : 1
      blank(from, i)
      if (i >= text.length) break
      const tokenStart = i
      if (text[i] === '`') { blank(i, i + 1); i++; update('`', '`', tokenStart) } else { blank(i, i + 2); i += 2; update('${', '${', tokenStart) }
      lastEnd = i
      continue
    }
    const c = text[i]
    if (/\s/.test(c) || c === '\ufeff') { i++; continue }
    if (c === '/' && text[i + 1] === '/') { const from = i; while (i < text.length && !'\n\r\u2028\u2029'.includes(text[i])) i++; blank(from, i); continue }
    if (c === '/' && text[i + 1] === '*') { const j = text.indexOf('*/', i + 2); const end = j < 0 ? text.length : j + 2; blank(i, end); i = end; continue }
    const tokenStart = i
    if (isIdentStart(c)) {
      while (i < text.length && isIdentPart(text[i])) i += text[i] === '\\' ? 2 : 1
      const word = text.slice(tokenStart, i)
      const type = beforeExprKeywords.has(word) || otherKeywords.has(word) || word === 'let' && false ? word : 'name'
      update(type, word, tokenStart)
    } else if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(text[i + 1] ?? ''))) {
      const m = text.slice(i).match(/^(?:0[xXoObB][0-9a-fA-F_]+n?|[0-9][0-9_]*n|(?:[0-9][0-9_]*\.?[0-9_]*|\.[0-9][0-9_]*)(?:[eE][+-]?[0-9_]+)?)/)
      i += m[0].length
      update('num', m[0], tokenStart)
    } else if (c === '"' || c === "'") {
      i++
      while (i < text.length && text[i] !== c) i += text[i] === '\\' ? 2 : 1
      i++
      blank(tokenStart, i)
      update('string', '', tokenStart)
    } else if (c === '`') {
      blank(i, i + 1); i++
      update('`', '`', tokenStart)
    } else if (c === '/' && exprAllowed) {
      i++
      let inClass = false
      while (i < text.length && !'\n\r\u2028\u2029'.includes(text[i])) {
        if (text[i] === '\\') { i += 2; continue }
        if (text[i] === '[') inClass = true
        else if (text[i] === ']') inClass = false
        else if (text[i] === '/' && !inClass) break
        i++
      }
      i++
      while (i < text.length && isIdentPart(text[i])) i++
      blank(tokenStart, i)
      update('regexp', '', tokenStart)
    } else {
      const op = punctuators.find(p => text.startsWith(p, i)) ?? c
      i += op.length
      update(op, op, tokenStart)
    }
    lastEnd = i
  }
  // Every brace, paren and template closes by the end of a module; a stack
  // left open means the tokenizer lost its place.
  return { code: out.join(''), balanced: stack.length === 1 }
}
const sourceSites = []
const programSites = []
for (const path of files(join(reference, 'shaders/src')).filter(path => path.endsWith('.js'))) {
  const rel = relative(join(reference, 'shaders/src'), path).replaceAll('\\', '/')
  const text = readFileSync(path, 'utf8')
  const { code, balanced } = codeOnly(text)
  if (!balanced) errors.push(`runtime: ${rel} did not tokenize cleanly`)
  const lineStarts = [0]
  for (let k = 0; k < text.length; k++) if (text[k] === '\n') lineStarts.push(k + 1)
  const lineOf = offset => { let lo = 0; let hi = lineStarts.length - 1; while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (lineStarts[mid] <= offset) lo = mid; else hi = mid - 1 } return lo + 1 }
  // The method each line belongs to: definitions sit at four spaces of indentation.
  const methods = []
  let method = null
  for (const line of code.split('\n')) {
    const definition = line.match(/^ {4}(?:async )?([A-Za-z_$][\w$]*)\s*\(.*\)\s*\{\s*$/)
    if (definition) method = definition[1]
    methods.push(method)
  }
  for (const match of code.matchAll(/\bcreateShaderModule\b/g)) {
    const line = lineOf(match.index)
    const site = `${rel}:${line}`
    const after = code.slice(match.index + match[0].length)
    const open = after.match(/^\s*\(/)
    if (!open) { errors.push(`runtime: ${site} uses createShaderModule other than by calling it`); continue }
    let depth = 0
    let k = open[0].length - 1
    for (; k < after.length; k++) {
      if (after[k] === '(') depth++
      else if (after[k] === ')' && --depth === 0) break
    }
    const argument = text.slice(match.index + match[0].length + open[0].length, match.index + match[0].length + k).replace(/\s+/g, '')
    if (rel === 'runtime/backends/webgpu.js' && ['compileComputeProgram', 'compileRenderProgram'].includes(methods[line - 1]) &&
        /^\{code:(source|vertexSource)\}$/.test(argument)) {
      programSites.push(site)
      continue
    }
    sourceSites.push(site)
  }
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
