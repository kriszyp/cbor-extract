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
console.log('extractStrings string creation passed')
