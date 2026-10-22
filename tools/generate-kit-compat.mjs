#!/usr/bin/env node
// generate-kit-compat.mjs: the export kit's effect list.
//
// Writes export-kit/compat-effects.json: a JSON array of the `<namespace>/<name>`
// ids of every effect in the catalog that parity/reference.json locks (its
// `effects/<namespace>/<name>.js` bundles; tools/catalog/fetch-catalog.mjs
// requires them to be exactly the effects of the locked manifest, and the build
// embeds each one), sorted. The kit builder reads it (export-kit/kit.config.json
// `compat.fromJsonList`) to write the kit's compat.json, the list of effects
// Noisedeck's export dialog lets this kit render. Every catalog effect renders
// here: scripts/parity-summary requires informative exact, strict or reviewed
// NEAR evidence for each one.
//
// Usage:
//   node tools/generate-kit-compat.mjs           write the file
//   node tools/generate-kit-compat.mjs --check   exit 1 if the file is stale

import { existsSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join, relative, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const LOCK = join(ROOT, 'parity', 'reference.json')
const OUT = join(ROOT, 'export-kit', 'compat-effects.json')

const args = process.argv.slice(2)
const check = args.includes('--check')
for (const arg of args) {
  if (arg !== '--check') {
    console.error(`generate-kit-compat: unknown argument ${arg}`)
    process.exit(2)
  }
}

const lock = JSON.parse(readFileSync(LOCK, 'utf8'))
const ids = []
for (const path of Object.keys(lock.files)) {
  const match = /^effects\/([A-Za-z0-9_-]+)\/([A-Za-z0-9_-]+)\.js$/.exec(path)
  if (match) ids.push(`${match[1]}/${match[2]}`)
}
if (!ids.length) {
  console.error(`generate-kit-compat: ${relative(ROOT, LOCK)} locks no effect bundles`)
  process.exit(1)
}
ids.sort()
const text = JSON.stringify(ids, null, 2) + '\n'

if (check) {
  const current = existsSync(OUT) ? readFileSync(OUT, 'utf8') : null
  if (current !== text) {
    console.error(`generate-kit-compat: ${relative(ROOT, OUT)} is stale; run node tools/generate-kit-compat.mjs`)
    process.exit(1)
  }
  console.log(`KIT-COMPAT: ${ids.length} effects, ${relative(ROOT, OUT)} is current`)
} else {
  writeFileSync(OUT, text)
  console.log(`KIT-COMPAT: ${ids.length} effects -> ${relative(ROOT, OUT)}`)
}
