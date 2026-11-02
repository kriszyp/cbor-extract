'use strict'
const test = require('node:test')
const assert = require('node:assert/strict')
const { extractStrings } = require('../index.js')
const { head, text, binary, document, check, random, randomDocument, MAX_STRINGS } = require('./cbor.cjs')

const extract = (...parts) => extractStrings(...document(...parts).args)

test('a single string is returned as a string, not an array', () => {
	assert.equal(extract('abc'), 'abc')
	assert.equal(extract('é!'), 'é!')
	assert.equal(extract(''), '')
	assert.equal(extract('b'.repeat(256)), 'b'.repeat(256))
})

test('nearby ASCII strings share one latin1 run that includes the bytes between them', () => {
	assert.equal(extract('abc', 'def'), 'abccdef')
	assert.equal(extract('abc', head(0, 0x102), 'def'), 'abc\x19\x01\x02cdef')
})

test('a gap of more than 40 bytes starts a new run', () => {
	// the gap is the byte string (2 byte header + content) plus the next text header
	assert.equal(extract('abc', binary(37), 'def'), 'abc' + binary(37).toString('latin1') + 'cdef') // 40 byte gap
	assert.deepEqual(extract('abc', binary(38), 'def'), ['abc', 'def']) // 41 byte gap
})

test('a run longer than 6000 bytes is split', () => {
	const strings = Array.from({ length: 30 }, (_, i) => String(i).padEnd(250, '.'))
	const doc = document(...strings)
	const result = extractStrings(...doc.args)
	assert.ok(Array.isArray(result) && result.every((run) => run.length <= 6000), 'runs stay within 6000 bytes')
	check(result, doc)
})

test('a non-ASCII string is its own element and flushes the run before it', () => {
	assert.deepEqual(extract('abc', 'é!', 'def'), ['abc', 'é!', 'def'])
	assert.deepEqual(extract('é', 'ab', 'cd'), ['é', 'abbcd'])
	assert.deepEqual(extract('中', '😀'), ['中', '😀'])
})

test('one byte of 0x80 or more anywhere in a short string makes it UTF-8, whatever its length and alignment', () => {
	// the ASCII check reads whole words, pairs of 4 bytes or single bytes depending on the length
	for (let length = 1; length <= 40; length++) {
		for (let at = 0; at < length; at++) {
			for (let offset = 0; offset < 8; offset++) {
				const bytes = Buffer.alloc(length, 'x')
				bytes[at] = 0xe9 // invalid UTF-8 on its own: decoded as U+FFFD, where latin1 would give é
				const doc = document('a'.repeat(offset + 1), { string: bytes }, 'b')
				const result = extractStrings(...doc.args)
				assert.deepEqual(result, ['a'.repeat(offset + 1), bytes.toString(), 'b'], `length ${length}, byte ${at}, offset ${offset}`)
			}
		}
		const doc = document('a', 'x'.repeat(length), 'b')
		assert.equal(extractStrings(...doc.args), doc.source.toString('latin1', 1), `ASCII, length ${length}`)
	}
})

test('a string of 256 bytes or more is decoded as UTF-8, outside any run', () => {
	const long = 'x'.repeat(256)
	assert.deepEqual(extract('ab', long, 'cd'), ['ab', long, 'cd'])
})

test('invalid UTF-8 is decoded with replacement characters, never as latin1', () => {
	for (const bytes of [[0x61, 0x80], [0x61, 0xff], [0xc3], [0xe2, 0x82]]) {
		const source = Buffer.concat([head(3, bytes.length), Buffer.from(bytes)])
		assert.equal(extractStrings(1, source.length, bytes.length, source), Buffer.from(bytes).toString(), Buffer.from(bytes).toString('hex'))
	}
})

test('every length encoding of text and byte strings is read', () => {
	for (const width of [0, 1, 2, 4]) {
		assert.deepEqual(extract('é', { string: 'abc', width }), ['é', 'abc'], `text, width ${width}`)
		assert.deepEqual(extract('é', binary(3, width), 'abc'), ['é', 'abc'], `bytes, width ${width}`)
	}
})

test('other tokens are skipped with their argument bytes', () => {
	for (const major of [0, 1, 4, 5, 6, 7]) {
		for (const n of [0, 23, 24, 0xff, 0x100, 0xffff, 0x10000]) {
			assert.deepEqual(extract('é', head(major, n), 'abc'), ['é', 'abc'], `major ${major}, argument ${n}`)
		}
		const eightByte = Buffer.from([(major << 5) | 27, 0, 0, 0, 0, 0, 0, 0, 1])
		assert.deepEqual(extract('é', eightByte, 'abc'), ['é', 'abc'], `major ${major}, 8 byte argument`)
	}
})

test('the scan starts at the given position and stops at the given size', () => {
	const doc = document(binary(5), 'abc', 'é', 'def')
	assert.deepEqual(extractStrings(doc.texts[1].start, doc.source.length, 2, doc.source), ['é', 'def'])
	assert.equal(extractStrings(doc.args[0], doc.texts[1].start - 1, 3, doc.source), 'abc')
})

test(`at most ${MAX_STRINGS} strings are extracted per call, ending on a whole string`, () => {
	const doc = document(...Array.from({ length: 300 }, (_, i) => `é${i}`))
	const result = extractStrings(...doc.args)
	assert.equal(result.length, MAX_STRINGS)
	assert.equal(result.at(-1), `é${MAX_STRINGS - 1}`)
	check(result, doc)
})

test('a byte string that reaches the end of the buffer ends the scan', () => {
	const doc = document('é', 'abc', head(2, 100))
	assert.deepEqual(extractStrings(...doc.args), ['é', 'abc'])
})

test('truncated input throws a TypeError', () => {
	const unexpectedEnd = { name: 'TypeError', message: 'Unexpected end of buffer reading string' }
	const doc = document('abc')
	assert.throws(() => extractStrings(1, doc.source.length - 1, 3, doc.source), unexpectedEnd, 'first string')
	for (const tail of [text('abcdef').subarray(0, 4), Buffer.from([0x78]), Buffer.from([0x79, 0]), Buffer.from([0x7a, 0, 0, 0])]) {
		assert.throws(() => extract('é', tail), unexpectedEnd, `tail ${tail.toString('hex')}`)
	}
	assert.throws(() => extract('é', Buffer.from([0x7b, 0, 0, 0, 0, 0, 0, 0])), unexpectedEnd, 'truncated 8 byte length')
	assert.throws(() => extract('é', Buffer.from([0x7b, 0, 0, 0, 1, 0, 0, 0, 0, 0x61])), unexpectedEnd, '8 byte length past the end')
	assert.throws(() => extractStrings(0, doc.source.length + 1, 0, doc.source), unexpectedEnd, 'size past the buffer')
	assert.throws(() => extractStrings(doc.source.length + 1, doc.source.length, 0, doc.source), unexpectedEnd, 'position past the buffer')
	assert.throws(() => extractStrings(3, 2, 0, doc.source), unexpectedEnd, 'position past the size')
})

// cbor-x reads these headers too, so a document using them must not fail only because the extractor is loaded
test('8 byte lengths are read like any other', () => {
	const eight = (major, length) => Buffer.from([(major << 5) | 27, 0, 0, 0, 0, 0, 0, 0, length])
	assert.deepEqual(extract('é', eight(3, 3), Buffer.from('abc')), ['é', 'abc'])
	assert.deepEqual(extract('é', eight(2, 3), Buffer.from([1, 2, 3]), 'abc'), ['é', 'abc'])
	assert.deepEqual(extract('é', Buffer.from([0x5b, 0xff, 0, 0, 0, 0, 0, 0, 0, 0x61])), 'é', 'a byte string past the end ends the scan')
})

test('an indefinite length string ends the scan, leaving it to cbor-x', () => {
	assert.equal(extract('é', Buffer.from([0x7f, 0x63, 0x61, 0x62, 0x63, 0xff])), 'é')
	assert.equal(extract('é', Buffer.from([0x5f, 0x41, 0x00, 0xff]), 'abc'), 'é')
	// indefinite arrays and maps have no argument bytes and are scanned through
	assert.deepEqual(extract('é', Buffer.from([0x9f, 0xbf]), 'abc', Buffer.from([0xff, 0xff])), ['é', 'abc'])
})

test('an empty string at position 0 is still returned', () => {
	assert.equal(extractStrings(0, 0, 0, Buffer.alloc(0)), '')
	assert.deepEqual(extractStrings(0, 3, 0, Buffer.from([0x62, 0xc3, 0xa9])), ['', 'é'])
	// the run starts at the empty string, so the next string's header is in it and cbor-x slices 'abc' from offset 1
	assert.equal(extractStrings(0, 4, 0, Buffer.from([0x63, 0x61, 0x62, 0x63])), 'cabc')
})

test('a run closed by a gap at the string limit still returns the run left open: 255 elements', () => {
	const doc = document(...Array(253).fill('é'), 'ab', binary(45), 'cd', 'é')
	const result = extractStrings(...doc.args)
	assert.equal(result.length, MAX_STRINGS + 1)
	assert.deepEqual(result.slice(-2), ['ab', 'cd'])
	check(result, doc)
})

test('arguments that are not non-negative integers throw a TypeError', () => {
	const source = text('abc')
	const invalid = { name: 'TypeError', message: 'Expected the position, size and string length to be non-negative integers' }
	for (const args of [[], ['1', 4, 3], [1, 4, undefined], [1n, 4, 3], [-1, 4, 3], [1.5, 4, 3], [1, NaN, 3], [1, Infinity, 3], [1, 4, 2 ** 53]]) {
		assert.throws(() => extractStrings(args[0], args[1], args[2], source), invalid, String(args))
	}
	// beyond 2^32 is rejected as past the buffer instead of wrapping to a small position
	assert.throws(() => extractStrings(2 ** 32 + 1, 2 ** 32 + 4, 3, source), { message: 'Unexpected end of buffer reading string' })
})

test('Array.of is captured when the module loads', () => {
	const arrayOf = Array.of
	Array.of = () => ['replaced']
	try {
		assert.deepEqual(extract('é', 'abc'), ['é', 'abc'])
	} finally {
		Array.of = arrayOf
	}
})

test('loads and works in worker threads', async () => {
	const { Worker } = require('node:worker_threads')
	const code = `
		const { parentPort } = require('node:worker_threads')
		const { extractStrings } = require(${JSON.stringify(require.resolve('../index.js'))})
		const source = Buffer.from([0x62, 0xc3, 0xa9, 0x63, 0x61, 0x62, 0x63])
		parentPort.postMessage(extractStrings(1, source.length, 2, source))`
	const results = await Promise.all(Array.from({ length: 4 }, () => new Promise((resolve, reject) => {
		const worker = new Worker(code, { eval: true })
		worker.once('message', (message) => worker.terminate().then(() => resolve(message)))
		worker.once('error', reject)
	})))
	for (const result of results) assert.deepEqual(result, ['é', 'abc'])
})

test('a source that is not a Buffer throws a TypeError', () => {
	for (const source of ['abc', 3, null, {}]) {
		assert.throws(() => extractStrings(0, 1, 0, source), { name: 'TypeError', message: 'Unexpected buffer type, expected a Buffer' })
	}
})

test('random documents decode to what cbor-x expects', () => {
	for (let seed = 1; seed <= 500; seed++) {
		const doc = randomDocument(random(seed))
		try {
			check(extractStrings(...doc.args), doc)
		} catch (error) {
			error.message = `seed ${seed}: ${error.message}`
			throw error
		}
	}
})
