module.exports = require('node-gyp-build')(__dirname)
let t = module.exports.testFast;

(function() {for (let i = 0; i < 100000;i++) {t.method()}})()

let start = process.hrtime()[1];
(function() {for (let i = 0; i < 1000000;i++) {t.method()}})()
 console.log('done', process.hrtime()[1] - start)

let b = Buffer.from('Hello, wolrd')
start = process.hrtime()[1];
(function() {for (let i = 0; i < 1000000;i++) {
	for (let j = 0, l = b.length;j < l; j++) {
		if (b[j] > 128)
			return false
	}
}})()
 console.log('done', process.hrtime()[1] - start)
