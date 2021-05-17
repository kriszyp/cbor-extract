/*
This is responsible for extracting the strings, in bulk, from a CBOR buffer. Creating strings from buffers can
be one of the biggest performance bottlenecks of parsing, but creating an array of extracting strings all at once
provides much better performance. This will parse and produce up to 256 strings at once .The JS parser can call this multiple
times as necessary to get more strings. This must be partially capable of parsing CBOR so it can know where to
find the string tokens and determine their position and length. All strings are decoded as UTF-8.
*/
#include <v8.h>
#include <node.h>
#include <node_buffer.h>
#include <nan.h>
using namespace v8;

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

class Extractor {
public:
	v8::Local<v8::Value> target[MAX_TARGET_SIZE + 1]; // leave one for the queued string

	uint8_t* source;
	int position = 0;
	int writePosition = 0;
	int stringStart = 0;
	int lastStringEnd = 0;
	Isolate *isolate = Isolate::GetCurrent();
	void readString(int length, bool allowStringBlocks) {
		int start = position;
		int end = position + length;
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
				target[writePosition++] = String::NewFromOneByte(isolate,  (uint8_t*) source + stringStart, v8::NewStringType::kNormal, lastStringEnd - stringStart).ToLocalChecked();
				lastStringEnd = 0;
			}
			// use standard utf-8 conversion
			target[writePosition++] = Nan::New<v8::String>((char*) source + start, (int) length).ToLocalChecked();
			position = end;
			return;
		}

		if (lastStringEnd) {
			if (start - lastStringEnd > 40 || end - stringStart > 6000) {
				target[writePosition++] = String::NewFromOneByte(isolate, (uint8_t*) source + stringStart, v8::NewStringType::kNormal, lastStringEnd - stringStart).ToLocalChecked();
				stringStart = start;
			}
		} else {
			stringStart = start;
		}
		lastStringEnd = end;
	}

	Local<Value> extractStrings(int startingPosition, int size, int firstStringSize, uint8_t* inputSource) {
		writePosition = 0;
		lastStringEnd = 0;
		position = startingPosition;
		source = inputSource;
		readString(firstStringSize, firstStringSize < 0x100);
		while (position < size) {
			uint8_t token = source[position++];
			uint8_t majorType = token >> 5;
			token = token & 0x1f;
			if (majorType == 2 || majorType == 3) {
				int length;
				switch (token) {
					case 0x18:
						if (position + 1 > size) {
							Nan::ThrowError("Unexpected end of buffer reading string");
							return Nan::Null();
						}
						length = source[position++];
						break;
					case 0x19:
						if (position + 2 > size) {
							Nan::ThrowError("Unexpected end of buffer reading string");
							return Nan::Null();
						}
						length = source[position++] << 8;
						length += source[position++];
						break;
					case 0x1a:
						if (position + 4 > size) {
							Nan::ThrowError("Unexpected end of buffer reading string");
							return Nan::Null();
						}
						length = source[position++] << 24;
						length += source[position++] << 16;
						length += source[position++] << 8;
						length += source[position++];
						break;
					case 0x1b:
						Nan::ThrowError("Unexpected end of buffer reading string");
						return Nan::Null();
						break;
					default:
						length = token;
				}
				if (majorType == 3) {
					// string
					if (length + position > size) {
						Nan::ThrowError("Unexpected end of buffer reading string");
						return Nan::Null();
					}
					readString(length, length < 0x100);
					if (writePosition >= MAX_TARGET_SIZE)
						break;
				} else { // binary data
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
			if (writePosition == 0)
				return String::NewFromOneByte(isolate, (uint8_t*) source + stringStart, v8::NewStringType::kNormal, lastStringEnd - stringStart).ToLocalChecked();
			target[writePosition++] = String::NewFromOneByte(isolate, (uint8_t*) source + stringStart, v8::NewStringType::kNormal, lastStringEnd - stringStart).ToLocalChecked();
		} else if (writePosition == 1) {
			return target[0];
		}
#if NODE_VERSION_AT_LEAST(12,0,0)
		return Array::New(isolate, target, writePosition);
#else
		Local<Array> array = Array::New(isolate, writePosition);
		Local<Context> context = Nan::GetCurrentContext();
		for (int i = 0; i < writePosition; i++) {
			array->Set(context, i, target[i]);
		}
		return array;
#endif
	}
};

static thread_local Extractor* extractor;

NAN_METHOD(extractStrings) {
	Local<Context> context = Nan::GetCurrentContext();
	int position = Local<Number>::Cast(info[0])->IntegerValue(context).FromJust();
	int size = Local<Number>::Cast(info[1])->IntegerValue(context).FromJust();
	int firstStringSize = Local<Number>::Cast(info[2])->IntegerValue(context).FromJust();
	if (info[3]->IsArrayBufferView()) {
		uint8_t* source = (uint8_t*) node::Buffer::Data(info[3]);
		info.GetReturnValue().Set(extractor->extractStrings(position, size, firstStringSize, source));
	}
}

NAN_METHOD(isOneByte) {
	info.GetReturnValue().Set(Nan::New<Boolean>(Local<String>::Cast(info[0])->IsOneByte()));
}

void initializeModule(v8::Local<v8::Object> exports) {
	extractor = new Extractor(); // create our thread-local extractor
	Nan::SetMethod(exports, "extractStrings", extractStrings);
	Nan::SetMethod(exports, "isOneByte", isOneByte);
}

NODE_MODULE_CONTEXT_AWARE(extractor, initializeModule);
