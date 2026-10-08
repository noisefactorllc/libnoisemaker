// check_midi_state.mjs -- MIDI state parity gate.
//
// Replays the same MIDI event scenarios through the REFERENCE MidiState
// (shaders/src/runtime/external-input.js, imported unchanged under Node with
// Date.now() stubbed to each event's timestamp) and through this port's
// nm::MidiState (build/midi_state_dump), then compares every state
// dump field by field. Numbers must be exactly equal: MidiState stores only
// integers and host timestamps, with no Float32Array or FFT arithmetic.
//
//   NM_REFERENCE_ROOT=/path/to/noisemaker node parity/check_midi_state.mjs
//
// Env: NM_MIDI_STATE_DUMP  candidate binary (default build/midi_state_dump)
//      NM_MIDI_FUZZ_EVENTS events per fuzz scenario (default 4000)

import { resolve, dirname, join } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { existsSync, mkdtempSync, writeFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { execFileSync } from 'node:child_process'

const HERE = dirname(fileURLToPath(import.meta.url))
const REPO = resolve(HERE, '..')
const DUMP = process.env.NM_MIDI_STATE_DUMP || join(REPO, 'build', 'nm-dump-midi-state')
const FUZZ_EVENTS = Number(process.env.NM_MIDI_FUZZ_EVENTS || 4000)

if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }
if (!existsSync(DUMP)) { console.error(`midi_state_dump not found at ${DUMP} (build the candidate first)`); process.exit(3) }

const REF = resolve(process.env.NM_REFERENCE_ROOT)
const { MidiState } = await import(pathToFileURL(join(REF, 'shaders/src/runtime/external-input.js')).href)

// ---------------------------------------------------------------- scenarios

function mulberry32(seed) {
    return () => {
        seed |= 0; seed = seed + 0x6D2B79F5 | 0
        let t = Math.imul(seed ^ seed >>> 15, 1 | seed)
        t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t
        return ((t ^ t >>> 14) >>> 0) / 4294967296
    }
}

const KEYS = { id: 'port-a', name: 'Keys' }
const PADS = { id: 'port-b', name: 'Pads' }
const KEYS2 = { id: 'port-c', name: 'Keys' }

function handcrafted() {
    let time = 1_700_000_000_000
    const events = []
    const msg = (data, port = null) => events.push({ op: 'message', data, port, time: time += 7 })
    const dump = () => events.push({ op: 'dump' })
    // Notes, running identity, velocity-0 note-off, clock.
    msg([0x90, 60, 100]); msg([0x90, 64, 90], KEYS); msg([0xf8]); msg([0xf8], PADS); dump()
    msg([0x90, 60, 0]); msg([0x80, 64, 0], KEYS); dump()
    // 14-bit CC pairs from two ports never mix bytes.
    msg([0xb0, 1, 64], KEYS); msg([0xb0, 33, 5], PADS); msg([0xb0, 33, 9], KEYS); dump()
    // Pitch bend, channel pressure (2 bytes), poly pressure.
    msg([0xe1, 0x7f, 0x7f], KEYS); msg([0xd1, 88], PADS); msg([0xa1, 60, 40], KEYS); dump()
    // RPN 0/0 data entry, increment, decrement; NRPN with LSB data.
    msg([0xb2, 101, 0], KEYS); msg([0xb2, 100, 0], KEYS); msg([0xb2, 6, 2], KEYS); msg([0xb2, 96, 0], KEYS)
    msg([0xb2, 97, 0], KEYS); msg([0xb2, 99, 1], PADS); msg([0xb2, 98, 2], PADS); msg([0xb2, 6, 3], PADS)
    msg([0xb2, 38, 17], PADS); msg([0xb2, 99, 127], PADS); msg([0xb2, 98, 127], PADS); msg([0xb2, 6, 5], PADS); dump()
    // MPE configuration via RPN 6 on channels 1 and 16, then zone notes.
    msg([0xb0, 101, 0], KEYS); msg([0xb0, 100, 6], KEYS); msg([0xb0, 6, 7], KEYS)
    msg([0xbf, 101, 0], PADS); msg([0xbf, 100, 6], PADS); msg([0xbf, 6, 9], PADS)
    msg([0x93, 62, 70], KEYS); msg([0x9e, 50, 60], PADS); dump()
    // All notes off, all sound off, reset all controllers.
    msg([0x91, 61, 30], PADS); msg([0xb1, 123, 0], PADS); msg([0xb1, 120, 0], KEYS); msg([0xb1, 11, 20], KEYS)
    msg([0xb1, 121, 0], KEYS); dump()
    // Invalid and ignored messages.
    msg([0x90]); msg([0x90, 200, 1]); msg([0x90, 60, 200]); msg([0xc0, 5]); msg([0xf0, 1, 2, 0xf7])
    msg([0x90, 60, 10], { id: '', name: 'nameless' }); dump()
    // Port topology: inventory, duplicate names, disconnect, reconnect.
    events.push({ op: 'inventory', ports: [KEYS, PADS, { ...KEYS2 }, { id: 'port-d', name: 'Gone', connected: false }] })
    msg([0x94, 70, 80], KEYS2); dump()
    events.push({ op: 'disconnect', id: 'port-a' }); dump()
    events.push({ op: 'register', port: { id: 'port-a', name: 'Keys Renamed' } }); msg([0x94, 71, 81], KEYS); dump()
    events.push({ op: 'reset' }); dump()
    return { name: 'handcrafted', events }
}

function fuzz(seed, count) {
    const rand = mulberry32(seed)
    const pick = (items) => items[Math.floor(rand() * items.length)]
    const ports = [null, KEYS, PADS, KEYS2, { id: 'port-e', name: 'Extra' }]
    const channels = [0, 1, 2, 3, 14, 15, 0, 1, 15, Math.floor(rand() * 16)]
    const controllers = [0, 1, 6, 7, 11, 32, 33, 38, 64, 74, 96, 97, 98, 99, 100, 101, 120, 121, 123]
    let time = 1_700_000_100_000
    const events = []
    for (let i = 0; i < count; i++) {
        time += Math.floor(rand() * 20)
        const port = pick(ports)
        const ch = pick(channels)
        const r = rand()
        let data
        if (r < 0.22) data = [0x90 | ch, 48 + Math.floor(rand() * 24), Math.floor(rand() * 128)]
        else if (r < 0.34) data = [0x80 | ch, 48 + Math.floor(rand() * 24), Math.floor(rand() * 128)]
        else if (r < 0.58) data = [0xb0 | ch, rand() < 0.8 ? pick(controllers) : Math.floor(rand() * 128), Math.floor(rand() * 128)]
        else if (r < 0.64) {
            // RPN 6 (MPE configuration) or a random RPN/NRPN transaction.
            const family = rand() < 0.5
            const mcm = rand() < 0.5
            const master = rand() < 0.5 ? 0 : 15
            const c = mcm ? master : ch
            const seq = family || mcm
                ? [[101, 0], [100, mcm ? 6 : Math.floor(rand() * 8)], [6, Math.floor(rand() * 17)]]
                : [[99, Math.floor(rand() * 4)], [98, Math.floor(rand() * 4)], [6, Math.floor(rand() * 128)], [38, Math.floor(rand() * 128)]]
            for (const [cc, value] of seq) events.push({ op: 'message', data: [0xb0 | c, cc, value], port, time })
            continue
        } else if (r < 0.70) data = [0xe0 | ch, Math.floor(rand() * 128), Math.floor(rand() * 128)]
        else if (r < 0.75) data = [0xd0 | ch, Math.floor(rand() * 128)]
        else if (r < 0.81) data = [0xa0 | ch, 48 + Math.floor(rand() * 24), Math.floor(rand() * 128)]
        else if (r < 0.85) data = [0xf8]
        else if (r < 0.88) data = pick([[0x90 | ch], [0x90 | ch, 130, 5], [0xb0 | ch, 7, 140], [0xc0 | ch, 3], [0xf0, 1, 0xf7]])
        else if (r < 0.90) { events.push({ op: 'disconnect', id: pick(ports.filter(Boolean)).id }); continue }
        else if (r < 0.92) { events.push({ op: 'register', port: pick(ports.filter(Boolean)) }); continue }
        else if (r < 0.93) {
            events.push({ op: 'inventory', ports: ports.filter(Boolean).map(p => ({ ...p, connected: rand() < 0.8 })) })
            continue
        } else if (r < 0.931) { events.push({ op: 'reset' }); continue }
        else data = [0x90 | ch, 48 + Math.floor(rand() * 24), 1 + Math.floor(rand() * 127)]
        events.push({ op: 'message', data, port, time })
        if (i % 200 === 199) events.push({ op: 'dump' })
    }
    events.push({ op: 'dump' })
    return { name: `fuzz-${seed}`, events }
}

const scenarios = [handcrafted(), fuzz(1, FUZZ_EVENTS), fuzz(2, FUZZ_EVENTS), fuzz(3, FUZZ_EVENTS)]

// ---------------------------------------------------------------- oracle

const originJson = (o) => (o === null || o === undefined) ? null : (typeof o === 'symbol' ? '<unscoped>' : `port:${o}`)
const mapObject = (map, fn = v => v) => Object.fromEntries([...map].map(([k, v]) => [String(k), fn(v)]))

function channelDump(ch) {
    return {
        key: ch.key, velocity: ch.velocity, gate: ch.gate, time: ch.time,
        keys: [...ch.keys], cc: [...ch.cc], cc14: [...ch.cc14],
        pitchBend: ch.pitchBend, pressure: ch.pressure, polyPressure: [...ch.polyPressure],
        nrpn: mapObject(ch.nrpn), rpn: mapObject(ch.rpn),
        heldNotes: mapObject(ch.heldNotes, n => ({ key: n.key, velocity: n.velocity, time: n.time, order: n.order, origin: originJson(n.origin) })),
        ccPorts: ch._ccPorts.map(originJson), cc14Ports: ch._cc14Ports.map(originJson),
        polyPressurePorts: ch._polyPressurePorts.map(originJson),
        pitchBendPort: originJson(ch._pitchBendPort), pressurePort: originJson(ch._pressurePort),
        nrpnPorts: mapObject(ch._nrpnPorts, originJson), rpnPorts: mapObject(ch._rpnPorts, originJson),
        selectors: { nrpn: [...ch._selectors.nrpn], rpn: [...ch._selectors.rpn] },
        parameterFamily: ch._parameterFamily
    }
}

function stateDump(state) {
    const out = {
        clockCount: state.clockCount,
        mpeZones: { lower: state.mpeZones.lower, upper: state.mpeZones.upper },
        channels: {}
    }
    for (let n = 1; n <= 16; n++) out.channels[String(n)] = channelDump(state.channels[n])
    if (!state._ports) return out
    out.ports = {}
    for (const entry of state._ports.values()) {
        out.ports[entry.id] = { id: entry.id, name: entry.name, connected: entry.connected, state: stateDump(entry.state) }
    }
    out.unscopedState = stateDump(state._unscopedState)
    if (state._portInventory) out.portInventory = mapObject(state._portInventory)
    const idOf = (s) => s === null ? null : [...state._ports.values()].find(e => e.state === s)?.id ?? null
    out.portsByName = mapObject(state._portsByName, idOf)
    return out
}

function runOracle(scenario) {
    const realNow = Date.now
    let now = 0
    Date.now = () => now
    try {
        const state = new MidiState()
        const dumps = []
        for (const event of scenario.events) {
            if (event.op === 'message') {
                now = event.time
                state.handleMessage(Uint8Array.from(event.data), event.port ?? undefined)
            } else if (event.op === 'disconnect') state.disconnectPort(event.id)
            else if (event.op === 'register') state.registerPort(event.port)
            else if (event.op === 'inventory') state.setPortInventory(event.ports)
            else if (event.op === 'reset') state.reset()
            else if (event.op === 'dump') dumps.push(stateDump(state))
        }
        return { name: scenario.name, dumps }
    } finally {
        Date.now = realNow
    }
}

// ---------------------------------------------------------------- compare

function firstDiff(a, b, path = '') {
    if (typeof a !== typeof b || Array.isArray(a) !== Array.isArray(b) || (a === null) !== (b === null)) {
        return `${path}: ${JSON.stringify(a)} vs ${JSON.stringify(b)}`
    }
    if (a === null || typeof a !== 'object') return Object.is(a, b) || a === b ? null : `${path}: ${JSON.stringify(a)} vs ${JSON.stringify(b)}`
    const keys = new Set([...Object.keys(a), ...Object.keys(b)])
    for (const k of keys) {
        if (!(k in a) || !(k in b)) return `${path}.${k}: ${k in a ? 'missing in candidate' : 'extra in candidate'}`
        const d = firstDiff(a[k], b[k], `${path}.${k}`)
        if (d) return d
    }
    return null
}

const oracle = scenarios.map(runOracle)
const dir = mkdtempSync(join(tmpdir(), 'nmq-midi-'))
let candidate
try {
    const file = join(dir, 'scenarios.json')
    writeFileSync(file, JSON.stringify(scenarios))
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
        const diff = actual ? firstDiff(expected.dumps[d], actual.dumps?.[d]) : 'missing scenario'
        if (diff) console.log(`[FAIL] ${expected.name} dump ${d}: ${diff}`)
        else pass++
    }
    const events = scenarios[i].events.length
    console.log(`[${expected.dumps.length === (actual?.dumps?.length ?? -1) ? 'INFO' : 'FAIL'}] ${expected.name}: ${events} events, ${expected.dumps.length} dumps`)
}
console.log(`MIDI_STATE: ${pass}/${total}`)
process.exit(pass === total ? 0 : 1)
