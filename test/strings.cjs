'use strict'
const assert = require('node:assert/strict')
const { extractStrings } = require(process.argv[2] || '../index.js')

const text = (string) => {
	const bytes = Buffer.from(string)
	return Buffer.concat([Buffer.from([0x60 | bytes.length]), bytes])
}
let source = Buffer.concat([text('abc'), text('é!')])
assert.deepEqual(extractStrings(1, source.length, 3, source), ['abc', 'é!'])
// more than 40 bytes between short strings flushes the first one
source = Buffer.concat([text('abc'), Buffer.alloc(41, 0x01), text('def')])
assert.deepEqual(extractStrings(1, source.length, 3, source), ['abc', 'def'])
source = Buffer.concat([Buffer.from([0x79, 0x01, 0x00]), Buffer.alloc(256, 0x62)])
assert.equal(extractStrings(3, source.length, 256, source), 'b'.repeat(256))
// a 64-bit length head (0x7b text, 0x5b bytes) after the first string is valid CBOR and must not fail the scan
const head64 = (major, length) => Buffer.concat([Buffer.from([major | 0x1b, 0, 0, 0, 0]), Buffer.from([length >>> 24, (length >>> 16) & 0xff, (length >>> 8) & 0xff, length & 0xff])])
source = Buffer.concat([text('ü'), head64(0x60, 1), Buffer.from('a')])
assert.deepEqual(extractStrings(1, source.length, 2, source), ['ü', 'a'])
source = Buffer.concat([text('abc'), head64(0x60, 3), Buffer.from('def')])
assert.equal(extractStrings(1, source.length, 3, source), source.toString('latin1', 1)) // latin strings come back as one block
source = Buffer.concat([text('ü'), head64(0x40, 2), Buffer.from([1, 2]), text('é')])
assert.deepEqual(extractStrings(1, source.length, 2, source), ['ü', 'é'])
// a 64-bit length over 32 bits can't fit in the buffer, so it still ends the scan
source = Buffer.concat([text('ü'), Buffer.from([0x7b, 0, 0, 0, 1, 0, 0, 0, 0]), Buffer.from('a')])
assert.throws(() => extractStrings(1, source.length, 2, source), /Unexpected end/)
console.log('extractStrings string creation passed')
