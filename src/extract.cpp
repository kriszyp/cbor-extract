/*
extractStrings(position, size, firstStringSize, buffer) for cbor-x. Creating JS strings one at a time is one of the
biggest costs of decoding, so cbor-x asks for the strings ahead of the one it is reading, and this creates up to 255 of
them in one call; cbor-x calls again when it needs more. scanner.hpp decides which bytes become which string; this file
reads the arguments, creates the strings and returns one string or an array of them.
*/
#include "napi_util.hpp"
#include "scanner.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cbor_extract {
namespace {

struct Arguments {
	std::span<const std::uint8_t> source; // the buffer up to size
	std::size_t position;
	std::size_t firstLength;
	napi_ref arrayOf;
};

Result<Arguments> readArguments(napi_env env, napi_callback_info info) {
	std::size_t argc = 4;
	std::array<napi_value, 4> args; // missing arguments come back as undefined
	void* data;
	if (auto ok = check(napi_get_cb_info(env, info, &argc, args.data(), nullptr, &data), Error::invalidArguments);
			!ok) {
		return std::unexpected(ok.error());
	}
	void* buffer;
	std::size_t bufferSize;
	if (auto ok = check(napi_get_buffer_info(env, args[3], &buffer, &bufferSize), Error::notABuffer); !ok) {
		return std::unexpected(ok.error());
	}
	const auto position = getIndex(env, args[0]);
	const auto size = getIndex(env, args[1]);
	const auto firstLength = getIndex(env, args[2]);
	if (!position || !size || !firstLength) return std::unexpected(Error::invalidArguments);
	// compared as doubles, before any conversion, so nothing past the buffer (or past 2^32 on 32-bit) is truncated
	if (*size > static_cast<double>(bufferSize) || *position > *size || *firstLength > *size - *position) {
		return std::unexpected(Error::unexpectedEnd);
	}
	return Arguments{
			.source = {static_cast<const std::uint8_t*>(buffer), static_cast<std::size_t>(*size)},
			.position = static_cast<std::size_t>(*position),
			.firstLength = static_cast<std::size_t>(*firstLength),
			.arrayOf = static_cast<napi_ref>(data),
	};
}

Result<napi_value> createString(napi_env env, std::span<const std::uint8_t> source, const StringSlice& slice) {
	const auto bytes = source.subspan(slice.start, slice.length);
	// a zero-length Buffer may have no data pointer, which Node-API rejects even for an empty string
	const char* chars = bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data());
	napi_value string;
	const napi_status status = slice.encoding == Encoding::latin1 ?
			napi_create_string_latin1(env, chars, bytes.size(), &string) :
			napi_create_string_utf8(env, chars, bytes.size(), &string);
	if (auto ok = check(status, Error::stringTooLong); !ok) return std::unexpected(ok.error());
	return string;
}

// The scan's sink: creates each string as soon as the scan finds it
class Strings {
public:
	Strings(napi_env env, std::span<const std::uint8_t> source) : env(env), source(source) {}

	bool operator()(const StringSlice& slice) {
		if (count == strings.size()) return fail(Error::tooManyStrings); // the scan stops at MAX_SLICES, so never
		const auto string = createString(env, source, slice);
		if (!string) return fail(string.error());
		strings[count++] = *string;
		return true;
	}
	// why the sink stopped the scan
	[[nodiscard]] Error error() const { return failure; }

	// the only string, or all of them in an array
	[[nodiscard]] Result<napi_value> toJs(napi_ref arrayOfRef) const {
		if (count == 1) return strings[0];
		// one Array.of call builds the result about twice as fast as napi_set_element per string
		napi_value arrayOf, undefined, array;
		if (napi_get_reference_value(env, arrayOfRef, &arrayOf) != napi_ok || arrayOf == nullptr ||
				napi_get_undefined(env, &undefined) != napi_ok ||
				napi_call_function(env, undefined, arrayOf, count, strings.data(), &array) != napi_ok) {
			return std::unexpected(Error::arrayFailed);
		}
		return array;
	}

private:
	napi_env env;
	std::span<const std::uint8_t> source;
	std::array<napi_value, MAX_SLICES> strings; // left uninitialized: only the first count are ever read
	std::size_t count = 0;
	Error failure = Error::arrayFailed;

	bool fail(Error error) {
		failure = error;
		return false;
	}
};
static_assert(SliceSink<Strings>);

napi_value extractStrings(napi_env env, napi_callback_info info) {
	const auto args = readArguments(env, info);
	if (!args) return throwError(env, args.error());
	Strings strings(env, args->source);
	const auto scanned = scanStrings(args->source, args->position, args->firstLength, strings);
	if (!scanned) {
		return throwError(env, scanned.error() == ScanError::rejected ? strings.error() : Error::unexpectedEnd);
	}
	const auto result = strings.toJs(args->arrayOf);
	return result ? *result : throwError(env, result.error());
}

// Array.of is captured once, at load time, so later changes to it do not affect results
Result<napi_value> init(napi_env env, napi_value exports) {
	napi_value global, arrayConstructor, arrayOf, function;
	napi_valuetype type;
	if (napi_get_global(env, &global) != napi_ok ||
			napi_get_named_property(env, global, "Array", &arrayConstructor) != napi_ok ||
			napi_get_named_property(env, arrayConstructor, "of", &arrayOf) != napi_ok ||
			napi_typeof(env, arrayOf, &type) != napi_ok || type != napi_function) {
		return std::unexpected(Error::noArrayOf);
	}
	napi_ref arrayOfRef;
	if (napi_create_reference(env, arrayOf, 1, &arrayOfRef) != napi_ok) return std::unexpected(Error::initFailed);
	// Node-API only unlinks a reference it did not create when the environment goes away, so the function deletes it
	// when it is collected or at teardown (once per Worker, or per load after clearing require.cache)
	constexpr auto deleteReference = [](napi_env env, void* data, void*) {
		napi_delete_reference(env, static_cast<napi_ref>(data));
	};
	if (napi_create_function(env, "extractStrings", NAPI_AUTO_LENGTH, extractStrings, arrayOfRef, &function) !=
					napi_ok ||
			napi_add_finalizer(env, function, arrayOfRef, deleteReference, nullptr, nullptr) != napi_ok) {
		napi_delete_reference(env, arrayOfRef);
		return std::unexpected(Error::initFailed);
	}
	if (auto ok = check(napi_set_named_property(env, exports, "extractStrings", function), Error::initFailed); !ok) {
		return std::unexpected(ok.error());
	}
	return exports;
}

} // namespace
} // namespace cbor_extract

NAPI_MODULE_INIT() {
	const auto result = cbor_extract::init(env, exports);
	return result ? *result : cbor_extract::throwError(env, result.error());
}
