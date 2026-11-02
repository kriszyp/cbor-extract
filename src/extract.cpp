/*
Extracts the strings of a CBOR buffer in bulk for cbor-x. Creating JS strings one at a time is one of the biggest costs of
decoding, so this scans ahead from the string cbor-x is reading and creates up to 255 strings in one call; cbor-x calls it
again when it needs more. Short ASCII strings that sit close together come back as one latin1 "run" that also holds the
bytes between them, which cbor-x slices by offset; every other string is its own UTF-8 element. It parses only as much
CBOR as it needs to find text strings: the length of byte and text strings, and how many argument bytes other tokens have.
*/
#include <node_api.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

constexpr size_t MAX_STRINGS = 254; // the scan stops once this many are created; one more slot holds the run left open
constexpr size_t SHORT_STRING = 0x100; // ASCII strings shorter than this can join a run
constexpr size_t MAX_GAP = 40; // most bytes between two strings of one run
constexpr size_t MAX_RUN = 6000; // most bytes in one run
constexpr double MAX_SAFE_INTEGER = 9007199254740991.0;

// throws unless an exception is already pending (a failed Node-API call may have thrown its own)
napi_value fail(napi_env env, const char* message, bool typeError = true) {
	bool pending = false;
	napi_is_exception_pending(env, &pending);
	if (!pending) {
		if (typeError) napi_throw_type_error(env, nullptr, message);
		else napi_throw_range_error(env, nullptr, message);
	}
	return nullptr;
}
napi_value unexpectedEnd(napi_env env) {
	return fail(env, "Unexpected end of buffer reading string");
}

// whole words first: about one branch per 8 bytes instead of one per byte
bool isAscii(const uint8_t* bytes, size_t length) {
	size_t i = 0;
	for (; i + 8 <= length; i += 8) {
		uint64_t word;
		std::memcpy(&word, bytes + i, 8);
		if (word & 0x8080808080808080ULL) return false;
	}
	for (; i < length; i++) {
		if (bytes[i] & 0x80) return false;
	}
	return true;
}

// bytes is 1, 2, 4 or 8; a switch lets each width compile to straight-line loads
uint64_t readBigEndian(const uint8_t* bytes, size_t width) {
	switch (width) {
		case 1: return bytes[0];
		case 2: return uint64_t(bytes[0]) << 8 | bytes[1];
		case 4: return uint64_t(bytes[0]) << 24 | uint64_t(bytes[1]) << 16 | uint64_t(bytes[2]) << 8 | bytes[3];
		default: return readBigEndian(bytes, 4) << 32 | readBigEndian(bytes + 4, 4);
	}
}

// lives on the stack for one call, so nothing is shared between calls, threads or environments
class Extractor {
public:
	Extractor(napi_env env, const uint8_t* source) : env(env), source(source) {}

	// position is the start of the first string, and firstLength fits before size; returns nullptr with an exception pending
	napi_value extract(size_t position, size_t size, size_t firstLength, napi_ref arrayOfRef) {
		// position, size and data stay locals so they live in registers: members could alias the bytes read through source.
		// Each switch case advances by a constant: a lookup table of widths would put a memory load in the chain from one
		// token's position to the next, where a jump the CPU predicts does not.
		const uint8_t* const data = source;
		// skipping a token's argument bytes advances at most 8 past a position below size, which must not wrap
		if (size > SIZE_MAX - 8) size = SIZE_MAX - 8;
		if (!readString(position, firstLength)) return nullptr;
		position += firstLength;
		while (position < size) {
			uint8_t token = data[position++];
			uint8_t majorType = token >> 5;
			uint8_t info = token & 0x1f;
			if (majorType == 2 || majorType == 3) {
				uint64_t length;
				switch (info) {
					case 24:
						if (size - position < 1) return unexpectedEnd(env);
						length = data[position];
						position += 1;
						break;
					case 25:
						if (size - position < 2) return unexpectedEnd(env);
						length = readBigEndian(data + position, 2);
						position += 2;
						break;
					case 26:
						if (size - position < 4) return unexpectedEnd(env);
						length = readBigEndian(data + position, 4);
						position += 4;
						break;
					case 27:
						if (size - position < 8) return unexpectedEnd(env);
						length = readBigEndian(data + position, 8);
						position += 8;
						break;
					case 28: case 29: case 30: case 31:
						goto done; // indefinite length or reserved: cbor-x handles (or rejects) it when it gets there
					default:
						length = info;
				}
				if (majorType == 3) {
					if (length > size - position) return unexpectedEnd(env);
					if (!readString(position, size_t(length))) return nullptr;
					position += size_t(length);
					if (count >= MAX_STRINGS) break;
				} else {
					if (length >= size - position) break; // reaches the end, so there is nothing after it to scan
					position += size_t(length);
				}
			} else { // the other major types only have argument bytes to skip; going past size just ends the loop
				switch (info) {
					case 24: position += 1; break;
					case 25: position += 2; break;
					case 26: position += 4; break;
					case 27: position += 8; break;
				}
			}
		}
	done:
		if (inRun) {
			napi_value run;
			if (!createString(runStart, runEnd - runStart, true, &run)) return nullptr;
			if (count == 0) return run;
			strings[count++] = run;
		} else if (count == 1) {
			return strings[0];
		}
		// one Array.of call builds the result about twice as fast as napi_set_element per string
		napi_value arrayOf, undefined, array;
		if (napi_get_reference_value(env, arrayOfRef, &arrayOf) != napi_ok || arrayOf == nullptr ||
				napi_get_undefined(env, &undefined) != napi_ok ||
				napi_call_function(env, undefined, arrayOf, count, strings, &array) != napi_ok) {
			return fail(env, "Unable to create the array of strings");
		}
		return array;
	}

private:
	napi_env env;
	const uint8_t* source;
	napi_value strings[MAX_STRINGS + 2]; // a string can flush the open run as well, and the last run is added at the end
	size_t count = 0;
	bool inRun = false; // runStart..runEnd is a latin1 run not created yet
	size_t runStart = 0;
	size_t runEnd = 0;

	bool createString(size_t start, size_t length, bool latin1, napi_value* value) {
		const char* chars = length ? reinterpret_cast<const char*>(source + start) : ""; // a zero-length Buffer may have no data
		napi_status status = latin1 ?
			napi_create_string_latin1(env, chars, length, value) :
			napi_create_string_utf8(env, chars, length, value);
		if (status != napi_ok) {
			fail(env, "Unable to create a string from the buffer", false); // longer than V8's maximum string length
			return false;
		}
		return true;
	}
	bool append(size_t start, size_t length, bool latin1) {
		if (!createString(start, length, latin1, &strings[count])) return false;
		count++;
		return true;
	}
	bool closeRun() {
		inRun = false;
		return append(runStart, runEnd - runStart, true);
	}
	// the caller checked that the string fits before size
	bool readString(size_t start, size_t length) {
		size_t end = start + length;
		// longer strings are not worth checking byte by byte: they go straight to UTF-8
		if (length < SHORT_STRING && isAscii(source + start, length)) {
			if (inRun && (start - runEnd > MAX_GAP || end - runStart > MAX_RUN)) {
				if (!closeRun()) return false;
			}
			if (!inRun) {
				inRun = true;
				runStart = start;
			}
			runEnd = end;
			return true;
		}
		if (inRun && !closeRun()) return false;
		return append(start, length, false);
	}
};

// a non-negative integer argument, read as a double so values past 2^32 are rejected instead of wrapping
bool getIndex(napi_env env, napi_value value, double* result) {
	return napi_get_value_double(env, value, result) == napi_ok &&
		*result >= 0 && *result <= MAX_SAFE_INTEGER && std::floor(*result) == *result;
}

// extractStrings(position, size, firstStringSize, buffer)
napi_value extractStrings(napi_env env, napi_callback_info info) {
	size_t argc = 4;
	napi_value args[4]; // missing arguments come back as undefined
	void* data;
	if (napi_get_cb_info(env, info, &argc, args, nullptr, &data) != napi_ok) return fail(env, "Unable to read the arguments");
	void* source;
	size_t bufferSize;
	if (napi_get_buffer_info(env, args[3], &source, &bufferSize) != napi_ok) return fail(env, "Unexpected buffer type, expected a Buffer");
	double position, size, firstStringSize;
	if (!getIndex(env, args[0], &position) || !getIndex(env, args[1], &size) || !getIndex(env, args[2], &firstStringSize)) {
		return fail(env, "Expected the position, size and string length to be non-negative integers");
	}
	if (size > double(bufferSize) || position > size || firstStringSize > size - position) return unexpectedEnd(env);
	Extractor extractor(env, static_cast<const uint8_t*>(source));
	return extractor.extract(size_t(position), size_t(size), size_t(firstStringSize), static_cast<napi_ref>(data));
}

} // namespace

NAPI_MODULE_INIT() {
	// Array.of is captured once, at load time, so later changes to it do not affect results
	napi_value global, arrayConstructor, arrayOf, function;
	napi_valuetype type;
	napi_ref arrayOfRef;
	if (napi_get_global(env, &global) != napi_ok ||
			napi_get_named_property(env, global, "Array", &arrayConstructor) != napi_ok ||
			napi_get_named_property(env, arrayConstructor, "of", &arrayOf) != napi_ok ||
			napi_typeof(env, arrayOf, &type) != napi_ok || type != napi_function) {
		return fail(env, "cbor-extract needs Array.of");
	}
	if (napi_create_reference(env, arrayOf, 1, &arrayOfRef) != napi_ok) return fail(env, "Unable to initialize cbor-extract");
	// Node-API only unlinks a reference it did not create when the environment goes away, so the function deletes it
	// when it is collected or at teardown (once per Worker, or per load after clearing require.cache)
	auto deleteReference = [](napi_env env, void* data, void*) { napi_delete_reference(env, static_cast<napi_ref>(data)); };
	if (napi_create_function(env, "extractStrings", NAPI_AUTO_LENGTH, extractStrings, arrayOfRef, &function) != napi_ok ||
			napi_add_finalizer(env, function, arrayOfRef, deleteReference, nullptr, nullptr) != napi_ok) {
		napi_delete_reference(env, arrayOfRef);
		return fail(env, "Unable to initialize cbor-extract");
	}
	if (napi_set_named_property(env, exports, "extractStrings", function) != napi_ok) return fail(env, "Unable to initialize cbor-extract");
	return exports;
}
