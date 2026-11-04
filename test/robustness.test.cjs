'use strict'
// Malformed input must never hang or crash the process. extractStrings is synchronous native code, so each check runs
// in a child process: a hang hits the timeout and a crash shows up as a signal, instead of stalling or killing the runner.
const test = require('node:test')
const assert = require('node:assert/strict')
const { spawnSync } = require('node:child_process')

function inChild(name) {
	// generous: Node alone takes ~10s to start under QEMU armv7 emulation; a real hang never returns
	const child = spawnSync(process.execPath, [__filename, name], { timeout: 120000, stdio: 'inherit' })
	assert.equal(child.signal, null, `${name}: the child was killed (timeout or crash)`)
	assert.equal(child.status, 0)
}

const checks = {
	// a declared length must never move the scan position backwards (#5)
	'scan bounds'() {
		const { extractStrings } = require('../index.js')
		const prefix = Buffer.from([0x79, 0x01, 0x00]) // text string of 256 bytes
		for (const binaryHeader of [[0x5a, 0xff, 0xff, 0xff, 0xfb], [0x5a, 0xff, 0xff, 0xff, 0xff], [0x59, 0xff, 0xff]]) {
			const source = Buffer.concat([prefix, Buffer.alloc(256, 0x61), Buffer.from(binaryHeader), Buffer.from([0x01])])
			assert.equal(extractStrings(prefix.length, source.length, 256, source), 'a'.repeat(256))
		}
	},
	// random bytes and damaged documents either extract strings or throw the documented errors
	'malformed input'() {
		const { extractStrings } = require('../index.js')
		const { random, randomDocument } = require('./cbor.cjs')
		const rand = random(0x5eed)
		const int = (n) => Math.floor(rand() * n)
		for (let i = 0; i < 3000; i++) {
			let source
			if (i % 2) {
				source = Buffer.from(Array.from({ length: int(300) }, () => int(256)))
			} else {
				source = Buffer.from(randomDocument(rand).source)
				for (let flips = int(4); flips > 0; flips--) source[int(source.length)] = int(256)
				source = source.subarray(0, int(source.length + 1))
			}
			const position = int(source.length + 1)
			const args = [position, position + int(source.length - position + 1), int(300), source]
			try {
				const result = extractStrings(...args)
				assert.ok(typeof result === 'string' || (Array.isArray(result) && result.every((s) => typeof s === 'string')))
			} catch (error) {
				if (!(error instanceof TypeError || error instanceof RangeError)) {
					throw new Error(`case ${i}: ${error.message}, args ${args.slice(0, 3)} source ${source.toString('hex')}`)
				}
			}
		}
	},
}

if (process.argv[2]) {
	checks[process.argv[2]]()
} else {
	for (const name of Object.keys(checks)) test(name, () => inChild(name))
}
