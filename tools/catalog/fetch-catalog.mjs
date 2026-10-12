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

// Published effect bundles omit some shader source files, and the runtime's
// own shaders live in its modules. Cache both from the exact source commit so
// the catalog builder only reads cache. The bytes are the commit's blobs, not
// the working tree's, which local edits or checkout line-ending conversion can
// change. The stamp names the cache layout, so a cache filled by an earlier
// layout is filled again.
const sourceStamp = join(sourceCache, '.ready')
const stamp = `${lock.commit} committed effects-shaders+runtime-js`
let sourceReady = false
try { sourceReady = (await readFile(sourceStamp, 'utf8')).trim() === stamp } catch {}
if (!sourceReady) {
  if (offline) throw new Error(`pinned shader source is absent from cache: ${sourceCache}`)
  const reference = resolve(process.env.NM_REFERENCE_ROOT || join(root, '.cache/reference'))
  const cloned = !existsSync(join(reference, '.git'))
  if (cloned) {
    await mkdir(dirname(reference), { recursive: true })
    execFileSync('git', ['clone', '--filter=blob:none', '--no-checkout', lock.repository, reference], { stdio: 'inherit' })
  }
  const head = () => {
    try { return execFileSync('git', ['-C', reference, 'rev-parse', 'HEAD'], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim() } catch { return '' }
  }
  // A clone made here is checked out at the pin. This repository's own cache
  // also follows the lock; an existing checkout named by NM_REFERENCE_ROOT
  // belongs to the caller and must already be at the pin. A --no-checkout
  // clone can sit at the pin with an empty tree, so the manifest's presence
  // counts too.
  const populated = existsSync(join(reference, 'shaders/effects/manifest.json'))
  if (cloned || ((head() !== lock.commit || !populated) && !process.env.NM_REFERENCE_ROOT)) {
    execFileSync('git', ['-C', reference, 'fetch', '--quiet', '--filter=blob:none', 'origin', lock.commit], { stdio: 'inherit' })
    execFileSync('git', ['-C', reference, 'checkout', '--quiet', '--force', '--detach', lock.commit], { stdio: 'inherit' })
  }
  const actual = head()
  if (actual !== lock.commit) throw new Error(`reference checkout is ${actual}; expected ${lock.commit}`)
  // Paths and blobs at the commit: `git ls-tree` lists them, and one
  // `git cat-file --batch` reads them ("<sha> blob <size>\n<bytes>\n" each).
  const listed = execFileSync('git', ['-C', reference, 'ls-tree', '-r', '-z', '--name-only', lock.commit, '--',
    'shaders/effects', 'shaders/src'], { encoding: 'utf8', maxBuffer: 1 << 28 }).split('\0').filter(Boolean)
  const wanted = listed.filter(path => path.startsWith('shaders/effects/') ? /\.(wgsl|glsl)$/.test(path) : path.endsWith('.js'))
  const batch = execFileSync('git', ['-C', reference, 'cat-file', '--batch'], {
    input: wanted.map(path => `${lock.commit}:${path}`).join('\n') + '\n', maxBuffer: 1 << 30
  })
  await rm(sourceCache, { recursive: true, force: true })
  let offset = 0
  for (const path of wanted) {
    const headerEnd = batch.indexOf(0x0a, offset)
    const header = batch.subarray(offset, headerEnd).toString('utf8').split(' ')
    if (header[1] !== 'blob') throw new Error(`${path}: git cat-file returned ${header.join(' ')}`)
    const size = Number(header[2])
    const destination = join(sourceCache, path)
    await mkdir(dirname(destination), { recursive: true })
    await writeFile(destination, batch.subarray(headerEnd + 1, headerEnd + 1 + size))
    offset = headerEnd + 1 + size + 1
  }
  if (offset !== batch.length) throw new Error('git cat-file returned more output than requested')
  await writeFile(sourceStamp, stamp + '\n')
}
console.log(`CATALOG FETCH: ${paths.length} files, ${lock.version}, ${lock.commit}`)
