#include "server/admin/mv3d_sector_protocol.hpp"

#include <cassert>
#include <iostream>

using namespace mv3d::sector;

uint64_t readLE(const std::vector<uint8_t> &bytes, size_t offset, size_t count) {
	uint64_t value = 0;
	for (size_t i = 0; i < count; ++i) value |= uint64_t(bytes.at(offset + i)) << (8 * i);
	return value;
}

int main() {
	const auto request = decode({ 0x34, 0x12, 0x78, 0x56, 7, 3, 64 });
	assert(request.x == 0x1234 && request.y == 0x5678 && request.z == 7);
	assert(radius(request) == 3);
	assert(radius({ 0, 0, 0, 0, 0 }) == 1);
	assert(radius({ 0, 0, 0, 255, 0 }) == 16);
	assert(radius({ 0, 0, 0, 0, 10 }) == 5);
	const uint64_t id = 0x0000123456780007ULL;
	const auto empty = encode(request, {});
	assert(empty.size() == 37 && empty[0] == 0xA6 && empty[26] == 0xA8);
	assert(readLE(empty, 1, 8) == id && readLE(empty, 27, 8) == id);
	assert(readLE(empty, 9, 4) == 0 && readLE(empty, 13, 2) == 0);
	assert(empty[25] == 7 && empty.back() == 0);
	assert(encode(request, {}, 2).back() == 2);

	// Independent golden bytes catch changes in byte order, header size and field offsets.
	const auto one = encode(request, { { 0x1234, 0x5678, 7, 0xABCD } });
	const std::vector<uint8_t> expected {
		0xA6, 7, 0, 0x78, 0x56, 0x34, 0x12, 0, 0,
		7, 0, 0, 0, 1, 0, 0, 7, 0x34, 0x12, 0, 0, 0x78, 0x56, 0, 0, 7,
		0xA7, 7, 0, 0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0, 0, 0, 7, 0,
		0x34, 0x12, 0x78, 0x56, 7, 0xCD, 0xAB,
		0xA8, 7, 0, 0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0
	};
	assert(one == expected);

	// Parse back at exact packet boundaries, including the last short chunk.
	for (const size_t count : { size_t(197), size_t(198), size_t(65535), size_t(65536) }) {
		std::vector<Entry> entries(count, { 65535, 0, 15, 100 });
		const auto bytes = encode(request, entries);
		const size_t transmitted = std::min(count, size_t(65535));
		assert(readLE(bytes, 9, 4) == transmitted * 7);
		size_t offset = 26, decoded = 0, index = 0;
		while (bytes.at(offset) == 0xA7) {
			assert(readLE(bytes, offset + 1, 8) == id);
			assert(readLE(bytes, offset + 9, 2) == index++);
			const auto length = readLE(bytes, offset + 13, 2);
			assert(length > 0 && length % 7 == 0 && length + 15 <= 1400);
			assert(readLE(bytes, offset + 15, 2) == 65535);
			decoded += length / 7;
			offset += 15 + length;
		}
		assert(decoded == transmitted && readLE(bytes, 13, 2) == index);
		assert(bytes.at(offset) == 0xA8 && offset + 11 == bytes.size());
		assert(bytes.back() == (count > 65535 ? 1 : 0));
	}
	std::cout << "MV3D sector wire tests passed\n";
}
