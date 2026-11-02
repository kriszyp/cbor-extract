'use strict'
// Loads the binary from the optional platform package npm installed for this machine,
// or from a local `npm run build`. cbor-x falls back to pure JS if this throws.
const { platform, arch } = process
// the arm package is built for ARMv7; on ARMv6 it would crash with SIGILL instead of throwing
if (arch === 'arm' && process.config.variables.arm_version < 7) throw new Error('cbor-extract needs ARMv7 or newer')

function linuxLibc() {
	try {
		return require('fs').readFileSync('/usr/bin/ldd', 'latin1').includes('musl') ? 'musl' : 'glibc'
	} catch {
		// no ldd (distroless): only glibc builds of Node report a glibc version
		return process.report?.getReport().header.glibcVersionRuntime ? 'glibc' : 'musl'
	}
}

const file = platform === 'linux' ? `extract-${linuxLibc()}.node` : 'extract.node'
try {
	module.exports = require(`@cbor-extract/cbor-extract-${platform}-${arch}/${file}`)
} catch (error) {
	if (error.code !== 'MODULE_NOT_FOUND') throw error
	module.exports = require('./build/Release/extract.node')
}
