'use strict'
// node scripts/platform-package.cjs stage <target> <libc|-> <binary>
//   copies a built binary into node_modules/@cbor-extract/cbor-extract-<target>/ so index.js loads it in tests
// node scripts/platform-package.cjs finalize <dir>
//   turns every <dir>/cbor-extract-<target>/ into a package and sets the root optionalDependencies to exactly those
const fs = require('fs')
const path = require('path')
const root = path.join(__dirname, '..')
const main = JSON.parse(fs.readFileSync(path.join(root, 'package.json')))

function writeManifest(dir) {
	const [os, cpu] = path.basename(dir).replace('cbor-extract-', '').split('-')
	const expected = os === 'linux' ? ['extract-glibc.node', 'extract-musl.node'] : ['extract.node']
	const actual = fs.readdirSync(dir).filter((file) => file.endsWith('.node')).sort()
	const manifest = {
		name: `@cbor-extract/${path.basename(dir)}`,
		version: main.version,
		description: `${os}-${cpu} binary for ${main.name}`,
		license: main.license,
		repository: main.repository,
		os: [os],
		cpu: [cpu],
		files: expected,
	}
	fs.writeFileSync(path.join(dir, 'package.json'), JSON.stringify(manifest, null, 2) + '\n')
	return { manifest, expected, actual }
}

const [command, ...args] = process.argv.slice(2)
if (command === 'stage') {
	const [target, libc, binary] = args
	const dir = path.join(root, 'node_modules/@cbor-extract', `cbor-extract-${target}`)
	fs.mkdirSync(dir, { recursive: true })
	fs.copyFileSync(binary, path.join(dir, libc === '-' ? 'extract.node' : `extract-${libc}.node`))
	writeManifest(dir)
} else if (command === 'finalize') {
	const tag = process.env.GITHUB_REF_TYPE === 'tag' && process.env.GITHUB_REF_NAME
	if (tag && tag !== `v${main.version}`) throw new Error(`tag ${tag} does not match package.json version ${main.version}`)
	main.optionalDependencies = {}
	for (const name of fs.readdirSync(args[0]).sort()) {
		const { manifest, expected, actual } = writeManifest(path.join(args[0], name))
		if (expected.join() !== actual.join()) throw new Error(`${name} has ${actual}, expected ${expected}`)
		main.optionalDependencies[manifest.name] = main.version
	}
	if (Object.keys(main.optionalDependencies).length === 0) throw new Error('no platform packages found')
	fs.writeFileSync(path.join(root, 'package.json'), JSON.stringify(main, null, 2) + '\n')
	console.log(main.optionalDependencies)
} else {
	throw new Error(`unknown command ${command}`)
}
