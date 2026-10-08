#!/usr/bin/env node
import { createHash } from 'node:crypto'
import { execFileSync } from 'node:child_process'
import { existsSync } from 'node:fs'
import { mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..')
const lockPath = join(root, 'parity/reference.json')
const args = process.argv.slice(2)
const update = args.includes('--update-lock')
const offline = args.includes('--offline')
const cacheArg = args.indexOf('--cache')
if (cacheArg >= 0 && !args[cacheArg + 1]) throw new Error('--cache requires a directory')
const cache = resolve(cacheArg < 0 ? join(root, '.cache/catalog') : args[cacheArg + 1])
const lock = JSON.parse(await readFile(lockPath, 'utf8'))
const versionDir = join(cache, lock.version)
const sourceCache = join(versionDir, 'source', lock.commit)

async function bytes (path) {
  if (offline) return readFile(join(versionDir, path))
  const response = await fetch(`${lock.cdn}/${lock.version}/${path}`)
  if (!response.ok) throw new Error(`${path}: HTTP ${response.status}`)
  const data = Buffer.from(await response.arrayBuffer())
  const destination = join(versionDir, path)
  await mkdir(dirname(destination), { recursive: true })
  await writeFile(destination, data)
  return data
}

const meta = JSON.parse((await bytes('deployment-meta.json')).toString('utf8'))
if (meta.git_hash !== lock.commit) {
  throw new Error(`deployment-meta.json: expected commit ${lock.commit}, got ${meta.git_hash}`)
}
const manifestBytes = await bytes('effects/manifest.json')
const manifest = JSON.parse(manifestBytes.toString('utf8'))
const ids = Object.keys(manifest).sort()
for (const id of ids) {
  if (!/^[A-Za-z0-9_-]+\/[A-Za-z0-9_-]+$/.test(id)) throw new Error(`invalid effect id ${id}`)
}
const paths = ['effects/manifest.json', ...ids.map(id => `effects/${id}.js`)].sort()
const hashes = {}
const queue = [...paths]
const workers = Array.from({ length: Math.min(12, queue.length) }, async () => {
  while (queue.length) {
    const path = queue.shift()
    const data = path === 'effects/manifest.json' ? manifestBytes : await bytes(path)
    const hash = createHash('sha256').update(data).digest('hex')
    hashes[path] = hash
    if (!update && lock.files[path] !== hash) {
      throw new Error(`${path}: sha256 ${hash}, lock ${lock.files[path] ?? '(missing)'}`)
    }
  }
})
await Promise.all(workers)
if (update) {
  lock.files = Object.fromEntries(Object.entries(hashes).sort(([a], [b]) => a.localeCompare(b)))
  await writeFile(lockPath, JSON.stringify(lock, null, 2) + '\n')
} else {
  const extra = Object.keys(lock.files).filter(path => !paths.includes(path))
  if (extra.length) throw new Error(`lock has unexpected files: ${extra.join(', ')}`)
}

// Published effect bundles omit some shader source files. Cache the missing
// bytes from the exact source commit so the catalog builder only reads cache.
const sourceStamp = join(sourceCache, '.ready')
let sourceReady = false
try { sourceReady = (await readFile(sourceStamp, 'utf8')).trim() === lock.commit } catch {}
if (!sourceReady) {
  if (offline) throw new Error(`pinned shader source is absent from cache: ${sourceCache}`)
  const reference = resolve(process.env.NM_REFERENCE_ROOT || join(root, '.cache/reference'))
  if (!existsSync(join(reference, '.git'))) {
    await mkdir(dirname(reference), { recursive: true })
    execFileSync('git', ['clone', '--filter=blob:none', '--no-checkout', lock.repository, reference], { stdio: 'inherit' })
    execFileSync('git', ['-C', reference, 'checkout', '--detach', lock.commit], { stdio: 'inherit' })
  }
  const actual = execFileSync('git', ['-C', reference, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim()
  if (actual !== lock.commit) throw new Error(`reference checkout is ${actual}; expected ${lock.commit}`)
  const sourceRoot = join(reference, 'shaders/effects')
  await rm(sourceCache, { recursive: true, force: true })
  async function copyShaders (relative = '') {
    for (const entry of await readdir(join(sourceRoot, relative), { withFileTypes: true })) {
      const path = join(relative, entry.name)
      if (entry.isDirectory()) await copyShaders(path)
      else if (entry.isFile() && /\.(wgsl|glsl)$/.test(entry.name)) {
        const destination = join(sourceCache, 'shaders/effects', path)
        await mkdir(dirname(destination), { recursive: true })
        await writeFile(destination, await readFile(join(sourceRoot, path)))
      }
    }
  }
  await copyShaders()
  await writeFile(sourceStamp, lock.commit + '\n')
}
console.log(`CATALOG FETCH: ${paths.length} files, ${lock.version}, ${lock.commit}`)
