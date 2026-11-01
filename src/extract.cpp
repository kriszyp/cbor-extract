/*
This is responsible for extracting the strings, in bulk, from a CBOR buffer. Creating strings from buffers can
be one of the biggest performance bottlenecks of parsing, but creating an array of extracting strings all at once
provides much better performance. This will parse and produce up to 256 strings at once .The JS parser can call this multiple
times as necessary to get more strings. This must be partially capable of parsing CBOR so it can know where to
find the string tokens and determine their position and length. All strings are decoded as UTF-8.
*/
#include <node_api.h>
#if ENABLE_V8_API
#include <v8.h>
#endif

#ifndef thread_local
#ifdef __GNUC__
# define thread_local __thread
#elif __STDC_VERSION__ >= 201112L
# define thread_local _Thread_local
#elif defined(_MSC_VER)
# define thread_local __declspec(thread)
#else
# define thread_local
#endif
#endif

const int MAX_TARGET_SIZE = 255;
napi_value unexpectedEnd(napi_env env) {
	napi_value returnValue;
	napi_get_undefined(env, &returnValue);
	napi_throw_type_error(env, NULL, "Unexpected end of buffer reading string");
	return returnValue;
}
class Extractor {
public:
	napi_value target[MAX_TARGET_SIZE + 1]; // leave one for the queued string

	uint8_t* source;
	uint32_t position = 0;
	uint32_t writePosition = 0;
	uint32_t stringStart = 0;
	uint32_t lastStringEnd = 0;

	// false with a RangeError pending when V8 rejects the string (longer than its maximum length)
	bool createString(napi_env env, uint32_t start, uint32_t length, bool latin1, napi_value* value) {
		napi_status status = latin1 ?
			napi_create_string_latin1(env, (const char*) source + start, length, value) :
			napi_create_string_utf8(env, (const char*) source + start, length, value);
		if (status != napi_ok) {
			napi_throw_range_error(env, NULL, "Unable to create a string from the buffer");
			return false;
		}
		return true;
	}
	bool appendString(napi_env env, uint32_t start, uint32_t length, bool latin1) {
		if (!createString(env, start, length, latin1, &target[writePosition]))
			return false;
		writePosition++;
		return true;
	}
	bool readString(napi_env env, uint32_t length, bool allowStringBlocks) {
		uint32_t start = position;
		uint32_t end = position + length;
		if (allowStringBlocks) { // for larger strings, we don't bother to check every character for being latin, and just go right to creating a new string
			while(position < end) {
				if (source[position] < 0x80) // ensure we character is latin and can be decoded as one byte
					position++;
				else {
					break;
				}
			}
		}
		if (position < end) {
			// non-latin character
			if (lastStringEnd) {
				if (!appendString(env, stringStart, lastStringEnd - stringStart, true))
					return false;
				lastStringEnd = 0;
			}
			// use standard utf-8 conversion
			if (!appendString(env, start, length, false))
				return false;
			position = end;
			return true;
		}

		if (lastStringEnd) {
			if (start - lastStringEnd > 40 || end - stringStart > 6000) {
				if (!appendString(env, stringStart, lastStringEnd - stringStart, true))
					return false;
				stringStart = start;
			}
		} else {
			stringStart = start;
		}
		lastStringEnd = end;
		return true;
	}
	napi_value extractStrings(napi_env env, uint32_t startingPosition, uint32_t size, uint32_t firstStringSize, uint8_t* inputSource) {
		writePosition = 0;
		lastStringEnd = 0;
		position = startingPosition;
		source = inputSource;
		if ((uint64_t)startingPosition + firstStringSize > size) {
			return unexpectedEnd(env);
		}
		if (!readString(env, firstStringSize, firstStringSize < 0x100))
			return NULL;
		while (position < size) {
			uint8_t token = source[position++];
			uint8_t majorType = token >> 5;
			token = token & 0x1f;
			if (majorType == 2 || majorType == 3) {
				uint32_t length;
				switch (token) {
					case 0x18:
						if (size - position < 1) {
							return unexpectedEnd(env);
						}
						length = source[position++];
						break;
					case 0x19:
						if (size - position < 2) {
							return unexpectedEnd(env);
						}
						length = source[position++] << 8;
						length += source[position++];
						break;
					case 0x1a:
						if (size - position < 4) {
							return unexpectedEnd(env);
						}
						length = (uint32_t) source[position++] << 24;
						length += source[position++] << 16;
						length += source[position++] << 8;
						length += source[position++];
						break;
					case 0x1b:
						return unexpectedEnd(env);
					default:
						length = token;
				}
				if (majorType == 3) {
					// string
					if (length > size - position) {
						return unexpectedEnd(env);
					}
					if (!readString(env, length, length < 0x100))
						return NULL;
					if (writePosition >= MAX_TARGET_SIZE - 1) // reserve slot for double-write
						break;
				} else { // binary data
					if (length >= size - position) // a declared length that reaches the end ends the scan, and can't wrap position
						break;
					position += length;
				}

			} else { // all other tokens
				switch (token) {
					case 0x18:
						position++;
						break;
					case 0x19:
						position += 2;
						break;
					case 0x1a:
						position += 4;
						break;
					case 0x1b:
						position += 8;
						break;
				}
			}
		}
		if (lastStringEnd) {
			napi_value value;
			if (!createString(env, stringStart, lastStringEnd - stringStart, true, &value))
				return NULL;
			if (writePosition == 0) {
				return value;
			}
			target[writePosition++] = value;
		} else if (writePosition == 1) {
			return target[0];
		}
		napi_value array;
		#if ENABLE_V8_API
		v8::Local<v8::Array> v8Array = v8::Array::New(v8::Isolate::GetCurrent(), (v8::Local<v8::Value>*) target, writePosition);
		memcpy(&array, &v8Array, sizeof(array));
		#else
		napi_create_array_with_length(env, writePosition, &array);
		for (int i = 0; i < writePosition; i++) {
			napi_set_element(env, array, i, target[i]);
		}
		#endif
		return array;
	}
};

static thread_local Extractor* extractor;

napi_value extractStrings(napi_env env, napi_callback_info info) {
	size_t argc = 4;
	napi_value args[4];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);
	uint32_t position;
	uint32_t size;
	uint32_t firstStringSize;
	napi_get_value_uint32(env, args[0], &position);
	napi_get_value_uint32(env, args[1], &size);
	napi_get_value_uint32(env, args[2], &firstStringSize);
	uint8_t* source;
	size_t buffer_size;
	napi_status status = napi_get_buffer_info(env, args[3], (void**) &source, &buffer_size);
	if (status) {
		napi_throw_type_error(env, NULL, "Unexpected buffer type, expected a Buffer");
		return args[0];
	}
	if (size > buffer_size || position > buffer_size) {
		return unexpectedEnd(env);
	}
	// fixed-size skips advance by at most 8 past a position below size, so this keeps them from wrapping
	if (size > UINT32_MAX - 8)
		size = UINT32_MAX - 8;
	return extractor->extractStrings(env, position, size, firstStringSize, source);
}
#define EXPORT_NAPI_FUNCTION(name, func) { napi_property_descriptor desc = { name, 0, func, 0, 0, 0, (napi_property_attributes) (napi_writable | napi_configurable), 0 }; napi_define_properties(env, exports, 1, &desc); }

NAPI_MODULE_INIT() {
	extractor = new Extractor(); // create our thread-local extractor
	EXPORT_NAPI_FUNCTION("extractStrings", extractStrings);
	return exports;
}