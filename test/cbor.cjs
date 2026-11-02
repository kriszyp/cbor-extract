'use strict'
// CBOR encoding helpers, a seeded document generator, and an oracle that checks extractStrings output the way cbor-x
// consumes it: an ASCII string shorter than 256 bytes is read by slicing a latin1 run that starts at the first string
// of the run and spans the source bytes up to the end of its last string; any other string is its own UTF-8 element.
const assert = require('node:assert/strict')

// width: the number of argument bytes (0, 1, 2 or 4), widened if n does not fit, so non-minimal headers are covered too
function head(major, n, width = 0) {
	width = Math.max(width, n < 24 ? 0 : n < 0x100 ? 1 : n < 0x10000 ? 2 : 4)
	const bytes = [(major << 5) | [n, 24, 25, , 26][width]]
	for (let i = width - 1; i >= 0; i--) bytes.push(Math.floor(n / 2 ** (8 * i)) & 0xff)
	return Buffer.from(bytes)
}
const text = (string, width) => {
	const bytes = Buffer.from(string)
	return Buffer.concat([head(3, bytes.length, width), bytes])
}
const binary = (length, width) => Buffer.concat([head(2, length, width), Buffer.alloc(length, 0xb1)])

// builds a document from parts (raw Buffers, strings, or { string, width } for a text item with a given header width)
// and returns the extractStrings arguments for it plus the position and bytes of every text item, in order
function document(...parts) {
	const chunks = []
	const texts = []
	let offset = 0
	for (const part of parts) {
		const string = typeof part === 'string' ? part : part.string
		const chunk = string === undefined ? part : text(string, part.width)
		if (string !== undefined) {
			const bytes = Buffer.from(string)
			texts.push({ start: offset + chunk.length - bytes.length, bytes })
		}
		chunks.push(chunk)
		offset += chunk.length
	}
	const source = Buffer.concat(chunks)
	return { source, texts, args: [texts[0].start, source.length, texts[0].bytes.length, source] }
}

const MAX_STRINGS = 254

function check(result, { source, texts }) {
	const output = typeof result === 'string' ? [result] : result
	assert.ok(Array.isArray(output), `expected a string or an array, got ${typeof result}`)
	let next = 0
	let run = null // the latin1 run being sliced: { start, string, end } where end is the end of its last string so far
	const closeRun = () => {
		if (run) assert.equal(run.string.length, run.end - run.start, `run at ${run.start} does not end at its last string`)
	}
	let covered = 0
	for (const { start, bytes } of texts) {
		const ascii = bytes.length < 0x100 && bytes.every((byte) => byte < 0x80)
		if (ascii && run && start + bytes.length <= run.start + run.string.length) {
			run.end = start + bytes.length
		} else {
			if (next === output.length) break
			closeRun()
			const value = output[next++]
			assert.equal(typeof value, 'string')
			if (ascii) {
				assert.equal(value, source.toString('latin1', start, start + value.length), `run at ${start} differs from the source`)
				run = { start, string: value, end: start + bytes.length }
			} else {
				assert.equal(value, bytes.toString(), `string at ${start}`)
				run = null
			}
		}
		covered++
	}
	closeRun()
	assert.equal(next, output.length, `${output.length - next} strings left over`)
	if (covered < texts.length) assert.ok(output.length >= MAX_STRINGS, `stopped after ${covered} of ${texts.length} strings`)
}

// mulberry32: small, seeded, and the same on every platform, so a failure names a seed that reproduces it
function random(seed) {
	return () => {
		seed = (seed + 0x6d2b79f5) | 0
		let t = Math.imul(seed ^ (seed >>> 15), 1 | seed)
		t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t
		return ((t ^ (t >>> 14)) >>> 0) / 4294967296
	}
}

const CHARACTERS = ['a', 'Z', '0', ' ', '~', '\x00', '\x7f', 'é', 'ÿ', 'Ж', '€', '中', '😀']
// a well-formed document of text strings mixed with byte strings and fixed-size tokens, starting with a text string
function randomDocument(rand) {
	const int = (n) => Math.floor(rand() * n)
	const pick = (array) => array[int(array.length)]
	const randomText = () => {
		const length = pick([0, 1, 5, 23, 24, 40, 100, 255, 256, 300])
		const ascii = rand() < 0.7
		let string = ''
		while (Buffer.byteLength(string) < length) string += ascii ? pick(CHARACTERS.slice(0, 7)) : pick(CHARACTERS)
		return string
	}
	const parts = [randomText()]
	for (let i = int(80); i > 0; i--) {
		const kind = int(10)
		if (kind < 4) parts.push(randomText())
		else if (kind < 5) parts.push({ string: randomText(), width: pick([1, 2, 4]) })
		else if (kind < 7) parts.push(binary(pick([0, 1, 10, 39, 40, 41, 100]), pick([0, 1, 2, 4])))
		else if (kind < 8) parts.push(Buffer.from([0x1b, 0, 0, 0, 0, 0, 0, 0, int(256)])) // 8 byte integer
		else parts.push(head(pick([0, 1, 4, 5, 6, 7]), int(70000))) // integers, array/map/tag heads, simple values
	}
	return document(...parts)
}

module.exports = { head, text, binary, document, check, random, randomDocument, MAX_STRINGS }
