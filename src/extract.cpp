/*
This is responsible for extracting the strings, in bulk, from a MessagePack buffer. Creating strings from buffers can
be one of the biggest performance bottlenecks of parsing, but creating an array of extracting strings all at once
provides much better performance. This will parse and produce up to 256 strings at once .The JS parser can call this multiple
times as necessary to get more strings. This must be partially capable of parsing MessagePack so it can know where to
find the string tokens and determine their position and length. All strings are decoded as UTF-8.
*/
#include <v8.h>
#include <node.h>
#include <node_buffer.h>
#include <nan.h>
using namespace v8;

const int MAX_TARGET_SIZE = 255;
typedef int (*token_handler)(uint8_t* source, int position, int size);
token_handler tokenTable[256] = {};
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
			if (start - lastStringEnd > 40 || end - stringStart > 4000) {
				target[writePosition++] = String::NewFromOneByte(isolate, (uint8_t*) source + stringStart, v8::NewStringType::kNormal, lastStringEnd - stringStart).ToLocalChecked();
				stringStart = start;
			}
		} else {
			stringStart = start;
		}
		lastStringEnd = end;
	}

	Local<Value> extractStrings(int startingPosition, int size, uint8_t* inputSource) {
		writePosition = 0;
		lastStringEnd = 0;
		position = startingPosition;
		source = inputSource;
		while (position < size) {
			uint8_t token = source[position++];
			uint8 majorType = token >> 5;
			token = token & 0x1f;
			if (majorType == 2 || majorType == 3) {
				int length;
				switch (token) {
					case 0x18:
						if (position + 1 > size) {
							Nan::ThrowError("Unexpected end of buffer");
							return size;
						}
						length = source[position++];
						break;
					case 0x19:
						if (position + 2 > size) {
							Nan::ThrowError("Unexpected end of buffer");
							return size;
						}
						int length = source[position++] << 8;
						length += source[position++];
						break;
					case 0x1a:
						if (position + 4 > size) {
							Nan::ThrowError("Unexpected end of buffer");
							return size;
						}
						int length = source[position++] << 24;
						length += source[position++] << 16;
						length += source[position++] << 8;
						length += source[position++];
						position += 4;
						break;
					case 0x1b:
						Nan::ThrowError("Too large of string/buffer");
						return size;
						break;
					default
						length = token;
				}
				if (majorType == 3) {
					// string
					if (length + position > size) {
						Nan::ThrowError("Unexpected end of buffer reading string");
						return Nan::Null();
					}
					readString(length, length < 0x100);
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
			if (writePosition >= MAX_TARGET_SIZE)
				break;
		}
	}
}

#ifdef thread_local
static thread_local Extractor* extractor;
#else
static Extractor* extractor;
#endif
NAN_METHOD(extractStrings) {
	Local<Context> context = Nan::GetCurrentContext();
	int position = Local<Number>::Cast(info[0])->IntegerValue(context).FromJust();
	int size = Local<Number>::Cast(info[1])->IntegerValue(context).FromJust();
	if (info[2]->IsArrayBufferView()) {
		uint8_t* source = (uint8_t*) node::Buffer::Data(info[2]);
		info.GetReturnValue().Set(extractor->extractStrings(position, size, source));
	}
}

NAN_METHOD(isOneByte) {
	Local<Context> context = Nan::GetCurrentContext();
	info.GetReturnValue().Set(Nan::New<Boolean>(Local<String>::Cast(info[0])->IsOneByte()));
}

void initializeModule(v8::Local<v8::Object> exports) {
	extractor = new Extractor(); // create our thread-local extractor
	setupTokenTable();
	Nan::SetMethod(exports, "extractStrings", extractStrings);
	Nan::SetMethod(exports, "isOneByte", isOneByte);
}

NODE_MODULE_CONTEXT_AWARE(extractor, initializeModule);
