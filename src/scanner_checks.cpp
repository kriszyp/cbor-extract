// Compile-time checks of the scanner: they run in the compiler, so the addon does not build if one fails, and they add
// nothing to the binary. test/ covers the same behaviour from JS, through Node-API.
#include "scanner.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace cbor_extract {
namespace {

struct Scanned {
	bool ok;
	std::vector<StringSlice> slices;
};

// scans source from position, the start of a text string of firstLength bytes; more than MAX_SLICES slices fails
constexpr Scanned scan(std::span<const std::uint8_t> source, std::size_t position, std::size_t firstLength) {
	Scanned scanned{false, {}};
	scanned.ok = scanStrings(source, position, firstLength, [&](const StringSlice& slice) {
		scanned.slices.push_back(slice);
		return scanned.slices.size() <= MAX_SLICES;
	}).has_value();
	return scanned;
}
constexpr Scanned scan(std::initializer_list<std::uint8_t> bytes, std::size_t position, std::size_t firstLength) {
	return scan(std::vector<std::uint8_t>(bytes), position, firstLength);
}
constexpr bool slicesAre(const Scanned& scanned, std::initializer_list<StringSlice> expected) {
	return scanned.ok && scanned.slices == std::vector<StringSlice>(expected);
}

constexpr auto latin1 = Encoding::latin1;
constexpr auto utf8 = Encoding::utf8;

// "abc" "def": one run from the first string to the end of the last, including the header between them
static_assert(slicesAre(scan({0x63, 'a', 'b', 'c', 0x63, 'd', 'e', 'f'}, 1, 3), {{1, 7, latin1}}));
// a non-ASCII string closes the run before it and is its own UTF-8 slice
static_assert(slicesAre(
		scan({0x61, 'a', 0x62, 0xc3, 0xa9, 0x61, 'b'}, 1, 1), {{1, 1, latin1}, {3, 2, utf8}, {6, 1, latin1}}));
// a 41 byte gap (2 byte header, 38 bytes, next header) starts a new run
static_assert([] {
	std::vector<std::uint8_t> bytes{0x61, 'a', 0x58, 38};
	bytes.resize(bytes.size() + 38);
	bytes.insert(bytes.end(), {0x61, 'b'});
	return slicesAre(scan(bytes, 1, 1), {{1, 1, latin1}, {bytes.size() - 1, 1, latin1}});
}());
// argument bytes of integers are skipped, and 8 byte lengths are read
static_assert(slicesAre(
		scan({0x61, 0xff, 0x19, 0x63, 0x63, 0x7b, 0, 0, 0, 0, 0, 0, 0, 1, 0xff}, 1, 1), {{1, 1, utf8}, {14, 1, utf8}}));
// an ASCII string of 256 bytes is its own UTF-8 slice, between runs
static_assert([] {
	std::vector<std::uint8_t> bytes{0x61, 'a', 0x79, 0x01, 0x00};
	bytes.resize(bytes.size() + 256, 'x');
	bytes.insert(bytes.end(), {0x61, 'b'});
	return slicesAre(scan(bytes, 1, 1), {{1, 1, latin1}, {5, 256, utf8}, {bytes.size() - 1, 1, latin1}});
}());
// the most slices one scan can return: 253 strings, then a run closed by a gap at the limit and the run left open
static_assert([] {
	std::vector<std::uint8_t> bytes;
	for (int i = 0; i < 253; i++)
		bytes.insert(bytes.end(), {0x62, 0xc3, 0xa9});
	bytes.insert(bytes.end(), {0x62, 'a', 'b', 0x58, 45});
	bytes.resize(bytes.size() + 45);
	bytes.insert(bytes.end(), {0x62, 'c', 'd', 0x62, 0xc3, 0xa9});
	const Scanned scanned = scan(bytes, 1, 2);
	return scanned.ok && scanned.slices.size() == MAX_SLICES &&
			scanned.slices.back() == StringSlice{bytes.size() - 5, 2, latin1};
}());
// an indefinite length string ends the scan
static_assert(slicesAre(scan({0x61, 0xff, 0x7f, 0x61, 'a', 0xff}, 1, 1), {{1, 1, utf8}}));
// a byte string that reaches the end ends the scan without an error
static_assert(slicesAre(scan({0x61, 0xff, 0x5a, 0xff, 0xff, 0xff, 0xff}, 1, 1), {{1, 1, utf8}}));
// an empty first string at position 0 is kept
static_assert(slicesAre(scan({}, 0, 0), {{0, 0, latin1}}));
// truncated strings, truncated length headers, and a first string past the end are errors
static_assert(!scan({0x63, 'a', 'b'}, 1, 3).ok);
static_assert(!scan({0x61, 'a', 0x63, 'b'}, 1, 1).ok);
static_assert(!scan({0x61, 'a', 0x79, 0x01}, 1, 1).ok);
static_assert(!scan({0x61, 'a', 0x7b, 0, 0, 0, 1, 0, 0, 0, 0}, 1, 1).ok);
static_assert(!scan({0x61, 'a'}, 3, 0).ok);
// a sink that returns false stops the scan at that slice
static_assert([] {
	const std::vector<std::uint8_t> bytes{0x61, 0xff, 0x61, 0xfe, 0x61, 0xfd};
	int calls = 0;
	const auto result = scanStrings(bytes, 1, 1, [&](const StringSlice&) { return ++calls < 2; });
	return !result && result.error() == ScanError::rejected && calls == 2;
}());

} // namespace
} // namespace cbor_extract
