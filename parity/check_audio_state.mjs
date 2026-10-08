// check_audio_state.mjs -- AudioState parity gate.
//
// Replays the same AudioState operations through the REFERENCE AudioState
// (shaders/src/runtime/external-input.js, imported unchanged under Node; the
// analyser is a stub that returns each event's recorded byte frequency data)
// and through this port's nm::AudioState (build/audio_state_dump),
// then compares every dump field by field. Numbers must be exactly equal:
// both sides use double arithmetic in the same order, and the Float32Array
// fields (fft, spectrum, waveform) round to the same float32 values.
//
//   NM_REFERENCE_ROOT=/path/to/noisemaker node parity/check_audio_state.mjs
// Env: NM_AUDIO_STATE_DUMP  candidate binary (default build/audio_state_dump)

import { resolve, dirname, join } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { existsSync, mkdtempSync, writeFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { execFileSync } from 'node:child_process'

const HERE = dirname(fileURLToPath(import.meta.url))
const REPO = resolve(HERE, '..')
const DUMP = process.env.NM_AUDIO_STATE_DUMP || join(REPO, 'build', 'nm-dump-audio-state')
if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }
if (!existsSync(DUMP)) { console.error(`audio_state_dump not found at ${DUMP} (build the candidate first)`); process.exit(3) }
const REF = resolve(process.env.NM_REFERENCE_ROOT)
const { AudioState } = await import(pathToFileURL(join(REF, 'shaders/src/runtime/external-input.js')).href)

function mulberry32(seed) {
    return () => {
        seed |= 0; seed = seed + 0x6D2B79F5 | 0
        let t = Math.imul(seed ^ seed >>> 15, 1 | seed)
        t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t
        return ((t ^ t >>> 14) >>> 0) / 4294967296
    }
}

// JSON-safe encoding of non-finite numbers.
const enc = (v) => Number.isNaN(v) ? 'NaN' : v === Infinity ? 'Infinity' : v === -Infinity ? '-Infinity' : v
const dec = (v) => v === 'NaN' ? NaN : v === 'Infinity' ? Infinity : v === '-Infinity' ? -Infinity : v

function scenario(seed, count) {
    const rand = mulberry32(seed)
    const pick = (items) => items[Math.floor(rand() * items.length)]
    const bytesOf = (n) => Array.from({ length: n }, () => rand() < 0.1 ? 0 : Math.floor(rand() * 256))
    const odd = () => pick([0.5, -0.2, 1.7, rand(), rand() * 3 - 1, 'NaN', 'Infinity', '-Infinity', 0, 1])
    const devices = [
        { id: 'mic', name: 'Mic', channelCount: 2 },
        { id: 'iface', name: 'Interface', channelCount: 8 },
        { id: 'iface-2', name: 'Interface', channelCount: 4 },
        { id: 'bad', name: 'Bad', channelCount: 0 }
    ]
    const events = []
    for (let i = 0; i < count; i++) {
        const r = rand()
        if (r < 0.25) events.push({ op: 'update', bytes: bytesOf(pick([128, 128, 1024, 16, 512])), smoothing: pick([3, 5, 1, 10, 12, 0, 2.5, 'NaN']) })
        else if (r < 0.33) {
            const target = rand() < 0.5 ? { channel: 1 + Math.floor(rand() * 9) } : { id: pick(devices).id, channel: 1 + Math.floor(rand() * 9) }
            events.push({ op: 'channelUpdate', ...target, bytes: bytesOf(128), smoothing: 3 })
        } else if (r < 0.38) events.push({ op: 'bands', low: odd(), mid: odd(), high: odd() })
        else if (r < 0.43) events.push({ op: 'raw', value: odd() })
        else if (r < 0.45) events.push({ op: 'rawUnavailable' })
        else if (r < 0.50) {
            const d = pick(devices)
            events.push({ op: 'registerDevice', device: { ...d, channelCount: rand() < 0.2 ? 1 + Math.floor(rand() * 6) : d.channelCount } })
        } else if (r < 0.62) {
            const values = {}
            for (const key of ['low', 'mid', 'high', 'vol', 'raw']) if (rand() < 0.6) values[key] = odd()
            events.push({ op: 'setChannelValues', id: pick(devices).id, channel: Math.floor(rand() * 10), values })
        } else if (r < 0.64) events.push({ op: 'deviceRawUnavailable', id: pick(devices).id })
        else if (r < 0.66) events.push({ op: 'disconnectDevice', id: pick(devices).id })
        else if (r < 0.68) events.push({ op: 'inventory', devices: devices.map(d => ({ ...d, connected: rand() < 0.8 })) })
        else if (r < 0.72) events.push({ op: 'registerDefault', count: pick([1, 2, 4, 8, 0, 33, 32]) })
        else if (r < 0.73) events.push({ op: 'disconnectDefault' })
        else if (r < 0.80) events.push({ op: 'spectrum', bytes: bytesOf(pick([128, 64, 300])) })
        else if (r < 0.87) events.push({ op: 'waveform', bytes: bytesOf(pick([256, 100])) })
        else if (r < 0.88) events.push({ op: 'resetAggregate' })
        else if (r < 0.885) events.push({ op: 'reset' })
        else events.push({ op: 'update', bytes: bytesOf(128), smoothing: 3 })
        if (i % 100 === 99) events.push({ op: 'dump' })
    }
    events.push({ op: 'dump' })
    return { name: `audio-${seed}`, events }
}

const scenarios = [scenario(1, 3000), scenario(2, 3000), scenario(3, 3000)]

// ---------------------------------------------------------------- oracle

const floatArray = (a) => [...a]
function dump(s) {
    const out = {
        low: s.low, mid: s.mid, high: s.high, vol: s.vol, raw: s.raw, rawReady: s.rawReady,
        fft: floatArray(s.fft), spectrum: floatArray(s.spectrum), waveform: floatArray(s.waveform),
        smoothing: { low: [...s._smoothingBuffers.low], mid: [...s._smoothingBuffers.mid], high: [...s._smoothingBuffers.high] },
        maxBufferLength: s._maxBufferLength
    }
    if (!s._devices) return out
    out.devices = {}
    for (const e of s._devices.values()) {
        out.devices[e.id] = {
            id: e.id, name: e.name, connected: e.connected, channelCount: e.channelCount,
            channels: Object.fromEntries([...e.channels].map(([n, st]) => [String(n), dump(st)]))
        }
    }
    out.defaultChannels = Object.fromEntries([...s._defaultChannels].map(([n, st]) => [String(n), dump(st)]))
    if (s._deviceInventory) out.deviceInventory = Object.fromEntries([...s._deviceInventory])
    out.devicesByName = Object.fromEntries([...s._devicesByName].map(([name, e]) => [name, e ? e.id : null]))
    out.defaultConnected = s._defaultConnected
    return out
}

function runOracle(sc) {
    const state = new AudioState()
    const dumps = []
    const stub = (bytes) => ({ frequencyBinCount: bytes.length, getByteFrequencyData: (buf) => buf.set(bytes) })
    for (const e of sc.events) {
        switch (e.op) {
        case 'update': state.updateFromAnalyser(stub(e.bytes), dec(e.smoothing)); break
        case 'channelUpdate': {
            const target = e.id !== undefined
                ? state.getDeviceChannelState({ id: e.id, name: e.id, channel: e.channel })
                : state.getDefaultChannelState(e.channel)
            target?.updateFromAnalyser(stub(e.bytes), dec(e.smoothing))
            break
        }
        case 'bands': state.setBands(dec(e.low), dec(e.mid), dec(e.high)); break
        case 'raw': state.setRaw(dec(e.value)); break
        case 'rawUnavailable': state.setRawUnavailable(); break
        case 'registerDevice': state.registerDevice(e.device); break
        case 'setChannelValues': state.setChannelValues(e.id, e.channel, Object.fromEntries(Object.entries(e.values).map(([k, v]) => [k, dec(v)]))); break
        case 'deviceRawUnavailable': state.setDeviceRawUnavailable(e.id); break
        case 'disconnectDevice': state.disconnectDevice(e.id); break
        case 'inventory': state.setDeviceInventory(e.devices); break
        case 'registerDefault': state.registerDefaultChannels(e.count); break
        case 'disconnectDefault': state.disconnectDefaultInput(); break
        case 'spectrum': state.setSpectrum(Uint8Array.from(e.bytes)); break
        case 'waveform': state.setWaveform(Uint8Array.from(e.bytes)); break
        case 'resetAggregate': state.resetAggregate(); break
        case 'reset': state.reset(); break
        case 'dump': dumps.push(JSON.parse(JSON.stringify(dump(state)))); break
        }
    }
    return { name: sc.name, dumps }
}

function firstDiff(a, b, path = '') {
    if (typeof a !== typeof b || Array.isArray(a) !== Array.isArray(b) || (a === null) !== (b === null)) {
        return `${path}: ${JSON.stringify(a)} vs ${JSON.stringify(b)}`
    }
    if (a === null || typeof a !== 'object') return a === b ? null : `${path}: ${JSON.stringify(a)} vs ${JSON.stringify(b)}`
    const keys = new Set([...Object.keys(a), ...Object.keys(b)])
    for (const k of keys) {
        if (!(k in a) || !(k in b)) return `${path}.${k}: ${k in a ? 'missing in candidate' : 'extra in candidate'}`
        const d = firstDiff(a[k], b[k], `${path}.${k}`)
        if (d) return d
    }
    return null
}

const oracle = scenarios.map(runOracle)
const dir = mkdtempSync(join(tmpdir(), 'nmq-audio-'))
let candidate
try {
    const file = join(dir, 'scenarios.json')
    writeFileSync(file, JSON.stringify(scenarios, (k, v) => typeof v === 'number' ? enc(v) : v))
    candidate = JSON.parse(execFileSync(DUMP, [file], { encoding: 'utf8', maxBuffer: 1 << 30 }))
} finally {
    rmSync(dir, { recursive: true, force: true })
}

let pass = 0
let total = 0
for (let i = 0; i < oracle.length; i++) {
    const expected = oracle[i]
    const actual = candidate[i]
    for (let d = 0; d < expected.dumps.length; d++) {
        total++
        const diff = firstDiff(expected.dumps[d], actual?.dumps?.[d])
        if (diff) console.log(`[FAIL] ${expected.name} dump ${d}: ${diff}`)
        else pass++
    }
    console.log(`[INFO] ${expected.name}: ${scenarios[i].events.length} events, ${expected.dumps.length} dumps`)
}
console.log(`AUDIO_STATE: ${pass}/${total}`)
process.exit(pass === total ? 0 : 1)
