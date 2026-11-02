// The CBOR scan behind extractStrings, kept free of Node-API so it is constexpr and checked at compile time
// (scanner_checks.cpp). It finds the text strings in a buffer and decides how each reaches cbor-x: short ASCII strings
// close together share one latin1 run, which also holds the bytes between them and which cbor-x slices by offset; every
// other string becomes its own UTF-8 string. It parses only as much CBOR as that needs: the length of byte and text
// strings, and how many argument bytes other tokens have.
#pragma once

#include <algorithm>
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

// an unaligned word from bytes, in native byte order
template <typename Word> [[nodiscard]] Word load(const std::uint8_t* bytes) {
	Word word;
	std::memcpy(&word, bytes, sizeof word);
	return word;
}

// data[start, start + length) has no byte >= 0x80. At run time it ORs bytes together and tests the high bits once,
// reading only inside the string: words that may overlap for 8 bytes or more, two 4 byte words that may overlap for 4
// to 7, and the first, middle and last byte for 1 to 3. Byte order does not matter for an OR.
[[nodiscard]] constexpr bool isAscii(const std::uint8_t* data, std::size_t start, std::size_t length) {
	const std::size_t end = start + length;
	if (!std::is_constant_evaluated()) {
		const auto wordAt = [data](std::size_t at) { return load<std::uint64_t>(data + at); };
		constexpr std::uint64_t highBits = 0x8080808080808080;
		if (length >= 8) {
			// the last word is tested first, so most non-ASCII strings stop there; the rest is ORed and tested once,
			// as a loop with no exit but its end runs faster, and strings here are under SHORT_STRING bytes
			std::uint64_t bits = wordAt(end - 8); // overlaps the last whole word when length is not a multiple of 8
			if (bits & highBits) return false;
			std::size_t at = start;
			for (; end - at > 16; at += 16)
				bits |= wordAt(at) | wordAt(at + 8);
			if (end - at > 8) bits |= wordAt(at);
			return (bits & highBits) == 0;
		}
		if (length >= 4)
			return ((load<std::uint32_t>(data + start) | load<std::uint32_t>(data + end - 4)) & 0x80808080) == 0;
		if (length == 0) return true;
		return ((data[start] | data[start + length / 2] | data[end - 1]) & 0x80) == 0;
	}
	for (std::size_t at = start; at < end; at++) {
		if (data[at] & 0x80) return false;
	}
	return true;
}

// Width is 1, 2, 4 or 8. Each is spelled out so it compiles to one load and byte swap: built up byte by byte in a
// 64-bit value, GCC loads 2 bytes separately.
template <std::size_t Width> [[nodiscard]] constexpr std::uint64_t readBigEndian(const std::uint8_t* bytes) {
	if constexpr (Width == 1) return bytes[0];
	else if constexpr (Width == 2) return static_cast<std::uint16_t>(bytes[0] << 8 | bytes[1]);
	else if constexpr (Width == 4) {
		return std::uint32_t{bytes[0]} << 24 | std::uint32_t{bytes[1]} << 16 | std::uint32_t{bytes[2]} << 8 | bytes[3];
	} else return readBigEndian<4>(bytes) << 32 | readBigEndian<4>(bytes + 4);
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
				std::uint64_t length;
				// 28 to 31 sit in the same switch as the widths, so the compiler dispatches on info with one jump
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
					case 28:
					case 29:
					case 30:
					case 31: return finish(); // indefinite length or reserved: cbor-x handles (or rejects) it there
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
		return finish();
	}

private:
	Sink& sink;
	std::size_t count = 0; // slices handed to the sink
	bool inRun = false;	   // runStart..runEnd is a latin1 run not added yet
	std::size_t runStart = 0;
	std::size_t runEnd = 0;

	// adds the run left open, if any
	constexpr std::expected<void, ScanError> finish() {
		if (inRun && !closeRun()) return std::unexpected(ScanError::rejected);
		return {};
	}
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
