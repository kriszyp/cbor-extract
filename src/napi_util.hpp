// Errors for the Node-API side. Functions return std::expected<T, Error>, and the callback turns an Error into the JS
// exception cbor-x sees at one place, throwError, so every failure path ends with exactly one exception pending.
#pragma once

#include <node_api.h>

#include <cmath>
#include <cstdint>
#include <expected>
#include <utility>

namespace cbor_extract {

enum class Error : std::uint8_t {
	unexpectedEnd,
	notABuffer,
	invalidArguments,
	stringTooLong,
	arrayFailed,
	tooManyStrings,
	initFailed,
	noArrayOf,
};

template <typename T> using Result = std::expected<T, Error>;

struct ErrorInfo {
	const char* message;
	bool range; // RangeError, otherwise TypeError
};

[[nodiscard]] constexpr ErrorInfo describe(Error error) {
	switch (error) {
		case Error::unexpectedEnd: return {"Unexpected end of buffer reading string", false};
		case Error::notABuffer: return {"Unexpected buffer type, expected a Buffer", false};
		case Error::invalidArguments:
			return {"Expected the position, size and string length to be non-negative integers", false};
		case Error::stringTooLong:
			return {"Unable to create a string from the buffer", true}; // past V8's maximum string length
		case Error::tooManyStrings: return {"More strings than cbor-extract can return at once", false};
		case Error::arrayFailed: return {"Unable to create the array of strings", false};
		case Error::initFailed: return {"Unable to initialize cbor-extract", false};
		case Error::noArrayOf: return {"cbor-extract needs Array.of", false};
	}
	std::unreachable();
}

// throws unless an exception is already pending (a failed Node-API call may have thrown its own); returns nullptr,
// which is what a callback or module init returns with an exception pending
inline napi_value throwError(napi_env env, Error error) {
	bool pending = false;
	napi_is_exception_pending(env, &pending);
	if (!pending) {
		const auto [message, range] = describe(error);
		if (range) napi_throw_range_error(env, nullptr, message);
		else napi_throw_type_error(env, nullptr, message);
	}
	return nullptr;
}

// napi_ok, or the given error
[[nodiscard]] inline Result<void> check(napi_status status, Error error) {
	if (status != napi_ok) return std::unexpected(error);
	return {};
}

// a non-negative integer, read as a double so values past 2^32 are rejected instead of wrapping
[[nodiscard]] inline Result<double> getIndex(napi_env env, napi_value value) {
	constexpr double maxSafeInteger = 9007199254740991.0;
	double number;
	if (napi_get_value_double(env, value, &number) != napi_ok || !(number >= 0 && number <= maxSafeInteger) ||
			std::floor(number) != number) {
		return std::unexpected(Error::invalidArguments);
	}
	return number;
}

} // namespace cbor_extract
