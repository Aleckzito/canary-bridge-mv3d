#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mv3d::sector {
using std::size_t;
constexpr size_t REQUEST_BYTES = 7;
constexpr size_t ENTRY_BYTES = 7;
constexpr size_t BEGIN_BYTES = 26;
constexpr size_t DATA_HEADER_BYTES = 15;
constexpr size_t END_BYTES = 11;
constexpr size_t MAX_ENTRIES_PER_CHUNK = (1400 - DATA_HEADER_BYTES) / ENTRY_BYTES;
constexpr size_t MAX_ENTRIES = 65535;

struct Request {
	uint16_t x = 0, y = 0;
	uint8_t z = 0, previewRadius = 0, sectorSizeTiles = 0;
};

struct Entry {
	uint16_t x = 0, y = 0;
	uint8_t z = 0;
	uint16_t itemId = 0;
};

inline Request decode(const std::array<uint8_t, REQUEST_BYTES> &bytes) {
	return { static_cast<uint16_t>(bytes[0] | (bytes[1] << 8)),
	         static_cast<uint16_t>(bytes[2] | (bytes[3] << 8)), bytes[4], bytes[5], bytes[6] };
}

inline int32_t radius(const Request &request) {
	return std::clamp<int32_t>(request.previewRadius ? request.previewRadius : request.sectorSizeTiles / 2, 1, 16);
}

inline void appendLE(std::vector<uint8_t> &bytes, uint64_t value, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		bytes.push_back(static_cast<uint8_t>(value & 0xFF));
		value >>= 8;
	}
}

// One contiguous transfer prevents broadcasts from splitting a packet header/body.
// status: 0=complete, 1=truncated, 2=invalid floor. Empty sectors still send BEGIN/END.
inline std::vector<uint8_t> encode(const Request &request, const std::vector<Entry> &entries, uint8_t status = 0) {
	const size_t count = std::min(entries.size(), MAX_ENTRIES);
	if (count != entries.size() && status == 0) status = 1;
	const size_t chunks = (count + MAX_ENTRIES_PER_CHUNK - 1) / MAX_ENTRIES_PER_CHUNK;
	const uint64_t id = (uint64_t(request.x) << 32) | (uint64_t(request.y) << 16) | request.z;
	std::vector<uint8_t> bytes;
	bytes.reserve(BEGIN_BYTES + count * ENTRY_BYTES + chunks * DATA_HEADER_BYTES + END_BYTES);
	bytes.push_back(0xA6);
	appendLE(bytes, id, 8);
	appendLE(bytes, count * ENTRY_BYTES, 4);
	appendLE(bytes, chunks, 2);
	bytes.push_back(0); // geometry channel
	bytes.push_back(request.z);
	appendLE(bytes, request.x, 4); // center, not a sector index
	appendLE(bytes, request.y, 4);
	bytes.push_back(static_cast<uint8_t>(2 * radius(request) + 1));
	for (size_t first = 0, index = 0; first < count; ++index) {
		const size_t batch = std::min(count - first, MAX_ENTRIES_PER_CHUNK);
		bytes.push_back(0xA7);
		appendLE(bytes, id, 8);
		appendLE(bytes, index, 2);
		bytes.push_back(0); // geometry channel
		bytes.push_back(0); // no compression
		appendLE(bytes, batch * ENTRY_BYTES, 2);
		for (size_t i = first; i < first + batch; ++i) {
			appendLE(bytes, entries[i].x, 2);
			appendLE(bytes, entries[i].y, 2);
			bytes.push_back(entries[i].z);
			appendLE(bytes, entries[i].itemId, 2);
		}
		first += batch;
	}
	bytes.push_back(0xA8);
	appendLE(bytes, id, 8);
	bytes.push_back(0);
	bytes.push_back(status);
	return bytes;
}
} // namespace mv3d::sector
