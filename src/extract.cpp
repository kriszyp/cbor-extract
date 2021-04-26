/*
This is responsible for extracting the strings, in bulk, from a CBOR buffer. Creating strings from buffers can
be one of the biggest performance bottlenecks of parsing, but creating an array of extracting strings all at once
provides much better performance. This will parse and produce up to 256 strings at once .The JS parser can call this multiple
times as necessary to get more strings. This must be partially capable of parsing CBOR so it can know where to
find the string tokens and determine their position and length. All strings are decoded as UTF-8.
*/
#include <v8.h>
#include <v8-fast-api-calls.h>
#include <node.h>
#include <node_buffer.h>
#include <nan.h>
using namespace v8;

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
			if (start - lastStringEnd > 40 || end - stringStart > 4000) {
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
							Nan::ThrowError("Unexpected end of buffer");
							return Nan::Null();
						}
						length = source[position++];
						break;
					case 0x19:
						if (position + 2 > size) {
							Nan::ThrowError("Unexpected end of buffer");
							return Nan::Null();
						}
						length = source[position++] << 8;
						length += source[position++];
						break;
					case 0x1a:
						if (position + 4 > size) {
							Nan::ThrowError("Unexpected end of buffer");
							return Nan::Null();
						}
						length = source[position++] << 24;
						length += source[position++] << 16;
						length += source[position++] << 8;
						length += source[position++];
						position += 4;
						break;
					case 0x1b:
						Nan::ThrowError("Too large of string/buffer");
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
		if (lastStringEnd)
			target[writePosition++] = String::NewFromOneByte(isolate, (uint8_t*) source + stringStart, v8::NewStringType::kNormal, lastStringEnd - stringStart).ToLocalChecked();
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

#ifdef thread_local
static thread_local Extractor* extractor;
#else
static Extractor* extractor;
#endif

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
	Local<Context> context = Nan::GetCurrentContext();
	info.GetReturnValue().Set(Nan::New<Boolean>(Local<String>::Cast(info[0])->IsOneByte()));
}
class CustomExternalOneByteStringResource : public String::ExternalOneByteStringResource {
private:
    const char *d;
    size_t l;

public:
    CustomExternalOneByteStringResource(char *data, size_t size) {
    // The Latin data
    this->d = data;
    // Number of Latin characters in the string
    this->l = size;
}
    ~CustomExternalOneByteStringResource() {};

	void Dispose() {
	    delete this;
	}
    const char *data() const{
	    return this->d;
	}

    size_t length() const {
	    return this->l;
	}

};

static CustomExternalOneByteStringResource* theResource;

NAN_METHOD(bufferToExternalString) {
	Local<Context> context = Nan::GetCurrentContext();
	CustomExternalOneByteStringResource* resource =
		new CustomExternalOneByteStringResource(node::Buffer::Data(info[0]), node::Buffer::Length(info[0]));
	//memcpy((void*) theResource->data(), source, length);
	auto str = Nan::New<v8::String>(resource).ToLocalChecked();
	info.GetReturnValue().Set(str);
}

static Persistent<String> theString;

NAN_METHOD(makeString) {
	Local<Context> context = Nan::GetCurrentContext();
	char* data = new char[1000];
	
	if (theString.IsEmpty()) {
		theResource = new CustomExternalOneByteStringResource(data, 1000);
		auto str = Nan::New<v8::String>(theResource).ToLocalChecked();
		theString.Reset(Isolate::GetCurrent(), str);
		info.GetReturnValue().Set(str);
	} else {
		theResource = new CustomExternalOneByteStringResource(data, 1000);
		info.GetReturnValue().Set(Nan::New<Boolean>(theString.Get(Isolate::GetCurrent())->MakeExternal(theResource)));
	}
}


char* sample = "Hello, world";
    // TODO(mslekova): Clean-up these constants
    // The constants kV8EmbedderWrapperTypeIndex and
    // kV8EmbedderWrapperObjectIndex describe the offsets for the type info
    // struct and the native object, when expressed as internal field indices
    // within a JSObject. The existance of this helper function assumes that
    // all embedder objects have their JSObject-side type info at the same
    // offset, but this is not a limitation of the API itself. For a detailed
    // use case, see the third example.
    static constexpr int kV8EmbedderWrapperTypeIndex = 0;
    static constexpr int kV8EmbedderWrapperObjectIndex = 1;

// Helper method with a check for field count.
    template <typename T, int offset>
    inline T* GetInternalField(v8::Local<v8::Object> wrapper) {
      return reinterpret_cast<T*>(
          wrapper->GetAlignedPointerFromInternalField(kV8EmbedderWrapperObjectIndex));
    }

    class CustomEmbedderType {
     public:
      // Returns the raw C object from a wrapper JS object.
      static CustomEmbedderType* Unwrap(v8::Local<v8::Object> wrapper) {
        return GetInternalField<CustomEmbedderType,
                                kV8EmbedderWrapperObjectIndex>(wrapper);
      }
      static void FastMethod(v8::ApiObject receiver_obj, int param) {
        /*v8::Object* v8_object = reinterpret_cast<v8::Object*>(&receiver_obj);
        CustomEmbedderType* receiver = static_cast<CustomEmbedderType*>(
          v8_object->GetAlignedPointerFromInternalField(kV8EmbedderWrapperObjectIndex));*/
        // Type checks are already done by the optimized code.
        // Then call some performance-critical method like:
        // receiver->Method(param);
      	for (int i = 0; i < 10; i++) {
      		if (sample[i] > 125)
      			fprintf(stderr,"f");
      	}
	}

      static void SlowMethod(
          const v8::FunctionCallbackInfo<v8::Value>& info) {
        /*v8::Local<v8::Object> instance =
          v8::Local<v8::Object>::Cast(info.Holder());
        CustomEmbedderType* receiver = Unwrap(instance);*/
        // TODO: Do type checks and extract {param}.
        //receiver->Method(param);
      	for (int i = 0; i < 10; i++) {
      		if (sample[i] > 125)
      			fprintf(stderr, "s");
      	}
      }
    };


    // The following setup function can be templatized based on
    // the {embedder_object} argument.
    Local<v8::Object> SetupCustomEmbedderObject() {

	Isolate *isolate = Isolate::GetCurrent();
	CustomEmbedderType* embedder_object = new CustomEmbedderType();

      v8::CFunction c_func =
        CFunction::Make(CustomEmbedderType::FastMethod);

      Local<v8::FunctionTemplate> method_template =
        v8::FunctionTemplate::New(
          isolate, CustomEmbedderType::SlowMethod, v8::Local<v8::Value>(),
          v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow,
          v8::SideEffectType::kHasNoSideEffect, &c_func);

      v8::Local<v8::ObjectTemplate> object_template =
        v8::ObjectTemplate::New(isolate);
      object_template->SetInternalFieldCount(
        kV8EmbedderWrapperObjectIndex + 1);
      object_template->Set(isolate, "method", method_template);

      // Instantiate the wrapper JS object.
      v8::Local<v8::Object> object =
          object_template->NewInstance(Nan::GetCurrentContext()).ToLocalChecked();
      object->SetAlignedPointerInInternalField(
        kV8EmbedderWrapperObjectIndex,
        reinterpret_cast<void*>(embedder_object));
      return object;
      // TODO: Expose {object} where it's necessary.
    }




void initializeModule(v8::Local<v8::Object> exports) {
	extractor = new Extractor(); // create our thread-local extractor
	Nan::SetMethod(exports, "extractStrings", extractStrings);
	Nan::SetMethod(exports, "isOneByte", isOneByte);
	Nan::SetMethod(exports, "bufferToExternalString", bufferToExternalString);
	Nan::SetMethod(exports, "makeString", makeString);
	exports->Set(Nan::GetCurrentContext(), Nan::New<String>("testFast").ToLocalChecked(), SetupCustomEmbedderObject());
}

NODE_MODULE_CONTEXT_AWARE(extractor, initializeModule);
