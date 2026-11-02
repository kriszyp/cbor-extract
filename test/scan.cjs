'use strict'
// a declared length must never move the scan position backwards; run in a child so a hang fails instead of stalling
const assert = require('node:assert/strict')
const { spawnSync } = require('node:child_process')
const addon = process.argv[2] || require.resolve('../index.js')

if (process.argv[3] === 'child') {
	const { extractStrings } = require(addon)
	const prefix = Buffer.from([0x79, 0x01, 0x00]) // text string of 256 bytes
	for (const binaryHeader of [[0x5a, 0xff, 0xff, 0xff, 0xfb], [0x5a, 0xff, 0xff, 0xff, 0xff], [0x59, 0xff, 0xff]]) {
		const source = Buffer.concat([prefix, Buffer.alloc(256, 0x61), Buffer.from(binaryHeader), Buffer.from([0x01])])
		assert.equal(extractStrings(prefix.length, source.length, 256, source), 'a'.repeat(256))
	}
	process.exit(0)
}
// generous: Node alone takes ~10s to start under QEMU armv7 emulation; a real hang never returns
const child = spawnSync(process.execPath, [__filename, addon, 'child'], { timeout: 120000, stdio: 'inherit' })
assert.equal(child.signal, null, 'extractStrings did not return')
assert.equal(child.status, 0)
console.log('extractStrings scan bounds passed')
