// The CBOR scan behind extractStrings, kept free of Node-API so it is constexpr and checked at compile time
// (scanner_checks.cpp). It finds the text strings in a buffer and decides how each reaches cbor-x: short ASCII strings
// close together share one latin1 run, which also holds the bytes between them and which cbor-x slices by offset; every
// other string becomes its own UTF-8 string. It parses only as much CBOR as that needs: the length of byte and text
// strings, and how many argument bytes other tokens have.
#pragma once

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>

namespace cbor_extract {

inline constexpr std::size_t MAX_STRINGS = 254;	   // the scan stops once this many slices are found
inline constexpr std::size_t SHORT_STRING = 0x100; // ASCII strings shorter than this can join a run
inline constexpr std::size_t MAX_GAP = 40;		   // most bytes between two strings of one run
inline constexpr std::size_t MAX_RUN = 6000;	   // most bytes in one run
// The last string read can pass MAX_STRINGS by one: either it closed a run and added itself (leaving no run open), or
// it closed a run and opened a new one, which is added at the end. scanner_checks.cpp reaches this.
inline constexpr std::size_t MAX_SLICES = MAX_STRINGS + 1;

enum class Encoding : std::uint8_t { latin1, utf8 };

// source[start, start + length) becomes one JS string
struct StringSlice {
	std::size_t start;
	std::size_t length;
	Encoding encoding;
	constexpr bool operator==(const StringSlice&) const = default;
};

enum class ScanError : std::uint8_t {
	unexpectedEnd, // a string or length header runs past the end of the data
	rejected,	   // the sink returned false
};

// Receives each slice as soon as it is found, in source order, at most MAX_SLICES per scan; returning false stops the
// scan. Handing slices over one at a time lets the caller create each string right away instead of collecting them
// first.
template <typename Sink>
concept SliceSink = std::invocable<Sink&, const StringSlice&> &&
		std::same_as<std::invoke_result_t<Sink&, const StringSlice&>, bool>;

namespace detail {

// data[start, start + length) has no byte >= 0x80. At run time it reads whole 8 byte words, about one branch per word
// instead of one per byte; a string of 1 to 7 bytes is checked with the one word that ends where it ends, masking off
// the bytes before it, which are still inside data whenever the string ends at 8 or later.
[[nodiscard]] constexpr bool isAscii(const std::uint8_t* data, std::size_t start, std::size_t length) {
	constexpr std::uint64_t highBits = 0x8080808080808080;
	const std::size_t end = start + length;
	if (!std::is_constant_evaluated() && std::endian::native == std::endian::little) {
		const auto wordAt = [data](std::size_t at) {
			std::uint64_t word;
			std::memcpy(&word, data + at, 8);
			return word;
		};
		if (length >= 8) {
			for (std::size_t at = start; at < end - 8; at += 8) {
				if (wordAt(at) & highBits) return false;
			}
			return (wordAt(end - 8) & highBits) == 0; // overlaps the previous word when length is not a multiple of 8
		}
		if (length == 0) return true;
		// little-endian: the string's bytes are the high length bytes of the word
		if (end >= 8) return (wordAt(end - 8) & highBits & (~std::uint64_t{0} << (8 * (8 - length)))) == 0;
	}
	for (std::size_t at = start; at < end; at++) {
		if (data[at] & 0x80) return false;
	}
	return true;
}

// compilers turn this into a single load and byte swap for each width
template <std::size_t Width> [[nodiscard]] constexpr std::uint64_t readBigEndian(const std::uint8_t* bytes) {
	std::uint64_t value = 0;
	for (std::size_t i = 0; i < Width; i++)
		value = (value << 8) | bytes[i];
	return value;
}

template <SliceSink Sink> class Scan {
public:
	constexpr explicit Scan(Sink& sink) : sink(sink) {}

	// position and size stay locals so they live in registers: members could alias the bytes read through data.
	// Each switch case advances by a constant: a lookup table of widths would put a memory load in the chain from one
	// token's position to the next, where a jump the CPU predicts does not.
	constexpr std::expected<void, ScanError> run(
			std::span<const std::uint8_t> source, std::size_t position, std::size_t firstLength) {
		// skipping a token's argument bytes advances at most 8 past a position below size, which must not wrap
		const std::size_t size = std::min(source.size(), std::numeric_limits<std::size_t>::max() - 8);
		const std::uint8_t* const data = source.data();
		if (position > size || firstLength > size - position) return std::unexpected(ScanError::unexpectedEnd);
		if (!readString(data, position, firstLength)) return std::unexpected(ScanError::rejected);
		position += firstLength;
		while (position < size) {
			const std::uint8_t token = data[position++];
			const std::uint8_t majorType = token >> 5;
			const std::uint8_t info = token & 0x1f;
			if (majorType == 2 || majorType == 3) {
				if (info > 27)
					break; // indefinite length or reserved: cbor-x handles (or rejects) it when it gets there
				std::uint64_t length;
				switch (info) {
					case 24:
						if (size - position < 1) return std::unexpected(ScanError::unexpectedEnd);
						length = readBigEndian<1>(data + position);
						position += 1;
						break;
					case 25:
						if (size - position < 2) return std::unexpected(ScanError::unexpectedEnd);
						length = readBigEndian<2>(data + position);
						position += 2;
						break;
					case 26:
						if (size - position < 4) return std::unexpected(ScanError::unexpectedEnd);
						length = readBigEndian<4>(data + position);
						position += 4;
						break;
					case 27:
						if (size - position < 8) return std::unexpected(ScanError::unexpectedEnd);
						length = readBigEndian<8>(data + position);
						position += 8;
						break;
					default: length = info;
				}
				if (majorType == 3) {
					if (length > size - position) return std::unexpected(ScanError::unexpectedEnd);
					if (!readString(data, position, static_cast<std::size_t>(length)))
						return std::unexpected(ScanError::rejected);
					position += static_cast<std::size_t>(length);
					if (count >= MAX_STRINGS) break;
				} else {
					if (length >= size - position) break; // reaches the end, so there is nothing after it to scan
					position += static_cast<std::size_t>(length);
				}
			} else { // the other major types only have argument bytes to skip; going past size just ends the loop
				switch (info) {
					case 24: position += 1; break;
					case 25: position += 2; break;
					case 26: position += 4; break;
					case 27: position += 8; break;
					default: break;
				}
			}
		}
		if (inRun && !closeRun()) return std::unexpected(ScanError::rejected);
		return {};
	}

private:
	Sink& sink;
	std::size_t count = 0; // slices handed to the sink
	bool inRun = false;	   // runStart..runEnd is a latin1 run not added yet
	std::size_t runStart = 0;
	std::size_t runEnd = 0;

	constexpr bool add(const StringSlice& slice) {
		count++;
		return sink(slice);
	}
	constexpr bool closeRun() {
		inRun = false;
		return add({runStart, runEnd - runStart, Encoding::latin1});
	}
	// the caller checked that the string fits in the data; false when the sink stopped the scan
	constexpr bool readString(const std::uint8_t* data, std::size_t start, std::size_t length) {
		const std::size_t end = start + length;
		// longer strings are not worth checking byte by byte: they go straight to UTF-8
		if (length < SHORT_STRING && isAscii(data, start, length)) {
			if (inRun && (start - runEnd > MAX_GAP || end - runStart > MAX_RUN) && !closeRun()) return false;
			if (!inRun) {
				inRun = true;
				runStart = start;
			}
			runEnd = end;
			return true;
		}
		if (inRun && !closeRun()) return false;
		return add({start, length, Encoding::utf8});
	}
};

} // namespace detail

// Finds the strings from position, the start of a text string of firstLength bytes, to the end of source, and hands
// each to sink. On success sink received at least one slice.
template <SliceSink Sink>
[[nodiscard]] constexpr std::expected<void, ScanError> scanStrings(
		std::span<const std::uint8_t> source, std::size_t position, std::size_t firstLength, Sink&& sink) {
	return detail::Scan<std::remove_reference_t<Sink>>(sink).run(source, position, firstLength);
}

} // namespace cbor_extract
