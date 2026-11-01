import pathlib
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class GoldenReadinessTests(unittest.TestCase):
    def run_node(self, body):
        result = subprocess.run(
            ["node", "--input-type=module", "-e", body], cwd=ROOT,
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_readiness_distinguishes_external_status_from_compile_failure(self):
        self.run_node(r'''
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
const source = readFileSync('parity/batch-golden.mjs', 'utf8')
const STATUS_TIMEOUT = 120000
const goldenBackend = 'webgpu'
const BACKEND_NAMES = { webgpu: 'webgpu' }
const runDsl = eval('(' + source.split('async function runDsl ')[1].split('// Register a fixture')[0].replace(/^\(/, 'async function (') + ')')
const src = 'roll().write(o0)'
const ready = { graph: { source: src, passes: [1, 2] }, isCompiling: false, backend: { getName: () => 'webgpu' } }
async function check(frames, shouldPass) {
 let examined = 0
 const page = {
  evaluate: async () => {},
  waitForFunction: async (predicate, args, options) => {
   assert.equal(options.timeout, STATUS_TIMEOUT)
   for (const [status, pipeline] of frames) {
    ++examined
    globalThis.document = { getElementById: () => ({ textContent: status }) }
    globalThis.window = { __noisemakerRenderingPipeline: pipeline }
    if (predicate(args)) return
   }
   throw Error('not ready')
  }
 }
 if (shouldPass) await runDsl(page, src, 2)
 else await assert.rejects(runDsl(page, src, 2))
 return examined
}
assert.equal(await check([['MIDI access failed', null], ['compiled successfully', ready]], true), 2)
assert.equal(await check([['audio input failed', null], ['compiled successfully', ready]], true), 2)
for (const status of ['compilation failed: invalid syntax', 'failed to load effects: missing shader']) {
 await assert.rejects(check([[status, ready], ['compiled successfully', ready]], true), /DSL compile failed/)
}
await check([['compiled successfully', { ...ready, graph: { source: 'other', passes: [1,2] } }]], false)
await check([['compiled successfully', { ...ready, isCompiling: true }]], false)
await check([['compiled successfully', { ...ready, backend: { getName: () => 'webgl2' } }]], false)
await check([['compiled successfully', { ...ready, graph: { source: src, passes: [1] } }]], false)
await check([['MIDI access failed', ready]], false)
''')

    def test_session_grants_real_midi_permissions_only_to_its_page_origin(self):
        self.run_node(r'''
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
const source = readFileSync('parity/batch-golden.mjs', 'utf8')
const start = source.indexOf('async function withSession ')
const end = source.indexOf('\n}', start) + 2
const code = source.slice(start, end).replace(/const \{ BrowserSession \} = await import[^\n]+/, '')
const REFERENCE_ROOT='/reference', VIEWER_PATH='/viewer', EFFECTS_DIR='/effects', GLOBALS_PREFIX='/globals', STATUS_TIMEOUT=120000
const calls=[]
const page={
 url:()=> 'http://127.0.0.1:34567/demo/shaders/',
 context:()=>({grantPermissions:async(...args)=>calls.push(args)}),
 setViewportSize:async()=>{},waitForFunction:async()=>{},evaluate:async()=> 'actual renderer'
}
class BrowserSession {
 constructor(){this.page=page}
 async setup(){}
 async setBackend(){assert.deepEqual(calls,[[['midi','midi-sysex'],{origin:'http://127.0.0.1:34567'}]])}
 async teardown(){calls.push('teardown')}
}
async function installCaptureRoute(){}
const withSession=eval('('+code+')')
await withSession({backend:'webgpu',size:256},async(s,p)=>assert.equal(p,page))
assert.equal(calls.length,2)
assert.equal(calls[1],'teardown')
''')
