'use strict'
// Micro-benchmark for extractStrings, comparing one or more builds of the addon on identical inputs.
//
//   node bench/extract.cjs build/Release/extract.node [other.node ...] [--rounds N] [--sample-ms N] [--only name,name]
//
// Each workload is a CBOR document built once from a seeded PRNG. A "pass" replays the calls cbor-x makes while decoding
// it: extractStrings at the first text string, then again at the first text string the previous result did not cover,
// until the document is done. Before timing, every binary must return deep-equal results for every call, or the script
// exits with status 1. Timing interleaves the binaries (rotating their order each round) so drift hits them all alike;
// each sample runs whole passes for about --sample-ms, and the reported figure is the median over --rounds samples.
// "vs first" compares medians; "paired" is the median and p10..p90 of the per-round ratio to the first binary, which is
// the better guide to whether a difference is real (a self-comparison shows the noise floor).
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const assert = require('node:assert/strict')

// ---- arguments

const files = []
const options = { rounds: 40, 'sample-ms': 60, only: '' }
for (let i = 2; i < process.argv.length; i++) {
	const arg = process.argv[i]
	if (arg.startsWith('--')) {
		const [name, inline] = arg.slice(2).split('=')
		if (!(name in options)) usage(`unknown option ${arg}`)
		options[name] = inline ?? process.argv[++i]
	} else files.push(arg)
}
if (files.length === 0) usage('no .node files given')
const ROUNDS = Number(options.rounds)
const SAMPLE_NS = Number(options['sample-ms']) * 1e6
if (!(ROUNDS > 0 && SAMPLE_NS > 0)) usage('--rounds and --sample-ms must be positive numbers')

function usage(message) {
	console.error(`${message}\nusage: node bench/extract.cjs a.node [b.node ...] [--rounds 40] [--sample-ms 60] [--only records,mixed]`)
	process.exit(2)
}

// Each binary is loaded from its own copy, so passing the same file twice gives two independent modules (their own
// thread-local state and code pages), the same as two different builds would.
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'cbor-extract-bench-'))
process.on('exit', () => fs.rmSync(tmp, { recursive: true, force: true }))
const binaries = files.map((file, i) => {
	const copy = path.join(tmp, `${i}-${path.basename(file)}`)
	fs.copyFileSync(path.resolve(file), copy)
	const module = { exports: {} }
	process.dlopen(module, copy)
	assert.equal(typeof module.exports.extractStrings, 'function', `${file} does not export extractStrings`)
	return { label: `[${i}] ${file}`, extractStrings: module.exports.extractStrings }
})

// ---- CBOR encoding

// mulberry32, as in test/cbor.cjs
function random(seed) {
	return () => {
		seed = (seed + 0x6d2b79f5) | 0
		let t = Math.imul(seed ^ (seed >>> 15), 1 | seed)
		t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t
		return ((t ^ (t >>> 14)) >>> 0) / 4294967296
	}
}

class Writer {
	constructor() {
		this.bytes = []
		this.texts = [] // { start, length, ascii } for every text string, start being the first content byte
	}
	head(major, n) {
		const width = n < 24 ? 0 : n < 0x100 ? 1 : n < 0x10000 ? 2 : 4
		this.bytes.push((major << 5) | [n, 24, 25, , 26][width])
		for (let i = width - 1; i >= 0; i--) this.bytes.push(Math.floor(n / 2 ** (8 * i)) & 0xff)
	}
	int(n) {
		if (n < 0) this.head(1, -1 - n)
		else this.head(0, n)
	}
	text(string) {
		const content = Buffer.from(string)
		this.head(3, content.length)
		this.texts.push({ start: this.bytes.length, length: content.length, ascii: content.length < 0x100 && content.every((b) => b < 0x80) })
		for (const byte of content) this.bytes.push(byte)
	}
	binary(content) {
		this.head(2, content.length)
		for (const byte of content) this.bytes.push(byte)
	}
	float64(x) {
		const b = Buffer.alloc(9)
		b[0] = 0xfb
		b.writeDoubleBE(x, 1)
		for (const byte of b) this.bytes.push(byte)
	}
	simple(value) {
		this.bytes.push(value === false ? 0xf4 : value === true ? 0xf5 : 0xf6)
	}
	done() {
		return { source: Buffer.from(this.bytes), texts: this.texts }
	}
}

const ASCII = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-.'
const CYRILLIC = 'абвгдежзийклмнопрстуфхцчшщыэюяАБВГДЕЖЗИЙКЛМН'
const CJK = '的一是不了人我在有他这中大来上国个到说们为子和你地出道也时年'
const EMOJI = ['😀', '🎉', '🚀', '👍', '❤️', '🔥', '✨', '🙂']

function workloads() {
	const list = []
	const add = (name, description, seed, build) => {
		const rand = random(seed)
		const int = (n) => Math.floor(rand() * n)
		const pick = (array) => array[int(array.length)]
		const ascii = (min, max) => {
			let s = ''
			for (let n = min + int(max - min + 1); n > 0; n--) s += ASCII[int(ASCII.length)]
			return s
		}
		const w = new Writer()
		build(w, { rand, int, pick, ascii })
		list.push({ name, description, ...w.done() })
	}

	// (a) JSON-like records: short ASCII keys, short ASCII values, small ints, booleans, the odd float
	add('records', 'maps of short ASCII keys, small ints, short ASCII values', 1, (w, { int, pick, ascii, rand }) => {
		const statuses = ['active', 'pending', 'closed', 'archived']
		w.head(4, 1500)
		for (let i = 0; i < 1500; i++) {
			w.head(5, 7)
			w.text('id'), w.int(100000 + i)
			w.text('name'), w.text(ascii(4, 16))
			w.text('email'), w.text(`${ascii(3, 10)}@example.com`)
			w.text('age'), w.int(18 + int(60))
			w.text('active'), w.simple(rand() < 0.5)
			w.text('status'), w.text(pick(statuses))
			w.text('score'), int(4) ? w.int(int(1000)) : w.float64(Math.round(rand() * 10000) / 100)
		}
	})

	// (b) short strings mixing ASCII, Cyrillic, CJK and emoji, as in user-generated text
	add('mixed-utf8', 'short strings, ASCII mixed with Cyrillic, CJK and emoji', 2, (w, { int, pick, ascii, rand }) => {
		const word = () => {
			const kind = int(4)
			let s = ''
			const n = 2 + int(10)
			if (kind === 0) return ascii(2, 12)
			const alphabet = kind === 1 ? CYRILLIC : kind === 2 ? CJK : null
			for (let i = 0; i < n; i++) s += alphabet ? alphabet[int(alphabet.length)] : rand() < 0.3 ? pick(EMOJI) : ascii(1, 3)
			return s
		}
		w.head(4, 1500)
		for (let i = 0; i < 1500; i++) {
			w.head(5, 3)
			w.text('user'), w.text(word())
			w.text('msg'), w.text(`${word()} ${word()}`)
			w.text('n'), w.int(int(500))
		}
	})

	// (c) a few long ASCII strings, each decoded on its own as UTF-8 since they are 256 bytes or more
	add('long-ascii', 'ASCII strings of 256..4000 bytes with small ints between', 3, (w, { int, ascii }) => {
		w.head(4, 60)
		for (let i = 0; i < 60; i++) {
			w.head(5, 2)
			w.text('body'), w.text(ascii(256, 4000))
			w.text('n'), w.int(int(100))
		}
	})

	// (d) adjacent ASCII strings just under 256 bytes, so runs grow until the 6000 byte split
	add('ascii-runs', 'adjacent 150..255 byte ASCII strings, runs split at 6000 bytes', 4, (w, { ascii }) => {
		w.head(4, 1000)
		for (let i = 0; i < 1000; i++) w.text(ascii(150, 255))
	})

	// (e) tiny strings where every other one is non-ASCII, so nearly each is its own element and calls stop at 254
	add('tiny-many', 'tiny strings alternating ASCII and non-ASCII, 254-string limit', 5, (w, { int, pick, ascii }) => {
		w.head(4, 4000)
		for (let i = 0; i < 4000; i++) w.text(i & 1 ? ascii(1, 3) : pick([CYRILLIC[int(CYRILLIC.length)], CJK[int(CJK.length)], 'é', pick(EMOJI)]))
	})

	// (f) mostly binary: byte strings of 64..4096 bytes, with an occasional short ASCII label
	add('binary-heavy', 'byte strings of 64..4096 bytes, an ASCII label every few', 6, (w, { int, rand, ascii }) => {
		w.head(4, 400)
		for (let i = 0; i < 400; i++) {
			if (i % 4 === 0) w.text(ascii(3, 12))
			const content = Buffer.alloc(64 + int(4033))
			for (let j = 0; j < content.length; j++) content[j] = int(256)
			w.binary(content)
			if (rand() < 0.5) w.int(int(70000))
		}
	})

	if (options.only) {
		const only = options.only.split(',')
		for (const name of only) if (!list.some((w) => w.name === name)) usage(`unknown workload ${name}`)
		return list.filter((w) => only.includes(w.name))
	}
	return list
}

// ---- the calls cbor-x makes

// how many of texts[from..] a result covers, the way cbor-x consumes it (see check() in test/cbor.cjs): an ASCII string
// under 256 bytes is sliced from the current latin1 run if the run reaches its end, anything else takes the next element
function covered(result, texts, from) {
	const output = typeof result === 'string' ? [result] : result
	let next = 0
	let runStart = -1
	let runEnd = -1
	let i = from
	for (; i < texts.length; i++) {
		const { start, length, ascii } = texts[i]
		if (ascii && runStart >= 0 && start + length <= runEnd) continue
		if (next === output.length) break
		const value = output[next++]
		if (ascii) (runStart = start), (runEnd = start + value.length)
		else runStart = -1
	}
	return i - from
}

function planCalls(workload, extractStrings) {
	const { source, texts } = workload
	const calls = []
	for (let i = 0; i < texts.length; ) {
		const args = [texts[i].start, source.length, texts[i].length, source]
		const n = covered(extractStrings(...args), texts, i)
		assert.ok(n > 0, `${workload.name}: call at ${args[0]} covered no strings`)
		calls.push(args)
		i += n
	}
	return calls
}

// ---- timing

let sink = 0
function pass(extractStrings, calls) {
	for (let i = 0; i < calls.length; i++) {
		const c = calls[i]
		sink += extractStrings(c[0], c[1], c[2], c[3]).length
	}
}

// runs whole passes for about SAMPLE_NS (using the pass count found during warm-up) and returns ns per pass
function sample(extractStrings, calls, passes) {
	const start = process.hrtime.bigint()
	for (let i = 0; i < passes; i++) pass(extractStrings, calls)
	return Number(process.hrtime.bigint() - start) / passes
}

const quantile = (values, q) => {
	const sorted = [...values].sort((a, b) => a - b)
	const at = (sorted.length - 1) * q
	const lo = Math.floor(at)
	return sorted[lo] + (sorted[Math.ceil(at)] - sorted[lo]) * (at - lo)
}
const median = (values) => quantile(values, 0.5)
const percent = (ratio) => `${ratio >= 1 ? '+' : ''}${((ratio - 1) * 100).toFixed(1)}%`

// ---- main

const list = workloads()
console.log(`node ${process.version}, ${os.cpus()[0]?.model ?? os.arch()}, ${ROUNDS} rounds of ~${SAMPLE_NS / 1e6} ms per binary per workload`)
binaries.forEach((b) => console.log(b.label))

let mismatch = false
for (const workload of list) {
	workload.calls = planCalls(workload, binaries[0].extractStrings)
	for (const [n, args] of workload.calls.entries()) {
		const expected = binaries[0].extractStrings(...args)
		for (const b of binaries.slice(1)) {
			try {
				assert.deepStrictEqual(b.extractStrings(...args), expected)
			} catch {
				console.error(`MISMATCH: ${workload.name}, call ${n} (position ${args[0]}): ${b.label} differs from ${binaries[0].label}`)
				mismatch = true
			}
		}
	}
}
if (mismatch) process.exit(1)
console.log('results identical across binaries for all workloads\n')

const started = Date.now()
const results = []
for (const workload of list) {
	const { calls, source } = workload
	// warm up every binary, then size a sample to about SAMPLE_NS
	let passes = 1
	for (const b of binaries) {
		const until = process.hrtime.bigint() + BigInt(Math.round(SAMPLE_NS * 2))
		let count = 0
		const start = process.hrtime.bigint()
		while (process.hrtime.bigint() < until) pass(b.extractStrings, calls), count++
		passes = Math.max(passes, Math.ceil((count * SAMPLE_NS) / Number(process.hrtime.bigint() - start)))
	}
	const samples = binaries.map(() => [])
	for (let round = 0; round < ROUNDS; round++) {
		for (let k = 0; k < binaries.length; k++) {
			const i = (round + k) % binaries.length
			samples[i].push(sample(binaries[i].extractStrings, calls, passes))
		}
	}
	results.push({ workload, samples })
}

console.log(
	list
		.map((w) => `${w.name.padEnd(13)} ${(w.source.length / 1024).toFixed(0).padStart(4)} KiB, ${String(w.texts.length).padStart(5)} strings, ${String(w.calls.length).padStart(3)} calls/pass: ${w.description}`)
		.join('\n') + '\n',
)
const header = ['workload', 'binary', 'ns/call', 'MB/s', 'spread', 'vs first', 'paired (p10..p90)']
const rows = []
for (const { workload, samples } of results) {
	const base = median(samples[0])
	samples.forEach((s, i) => {
		const m = median(s)
		const ratios = s.map((v, r) => v / samples[0][r])
		rows.push([
			i === 0 ? workload.name : '',
			`[${i}]`,
			(m / workload.calls.length).toFixed(0),
			(workload.source.length / m * 1e3).toFixed(0),
			`±${(((quantile(s, 0.9) - quantile(s, 0.1)) / 2 / m) * 100).toFixed(1)}%`,
			i === 0 ? '' : percent(m / base),
			i === 0 ? '' : `${percent(median(ratios))} (${percent(quantile(ratios, 0.1))}..${percent(quantile(ratios, 0.9))})`,
		])
	})
}
const widths = header.map((h, c) => Math.max(h.length, ...rows.map((r) => r[c].length)))
const line = (r) => r.map((cell, c) => (c < 2 ? cell.padEnd(widths[c]) : cell.padStart(widths[c]))).join('  ')
console.log(line(header))
rows.forEach((r) => console.log(line(r)))
console.log(`\ntimed for ${((Date.now() - started) / 1000).toFixed(1)} s (spread: half the p10..p90 range of a binary's samples; sink ${sink % 10})`)
