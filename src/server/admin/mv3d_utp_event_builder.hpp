#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace mv3d::utp {

inline std::string i32s(int32_t v) { return std::to_string(v); }
inline std::string u32s(uint32_t v) { return std::to_string(v); }
inline std::string u16s(uint16_t v) { return std::to_string(v); }
inline std::string u8s(uint8_t v) { return std::to_string(static_cast<int>(v)); }
inline std::string bools(bool v) { return v ? "true" : "false"; }

inline uint64_t nowMs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

inline std::string tsField() {
	return ",\"ts\":" + std::to_string(nowMs());
}

inline std::string fs(float v) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
	return buf;
}

inline std::string esc(const std::string &s) {
	std::string out;
	out.reserve(s.size() + 2);
	for (char c : s) {
		if (c == '"') {
			out += "\\\"";
		} else if (c == '\\') {
			out += "\\\\";
		} else {
			out += c;
		}
	}
	return out;
}

inline std::string BuildDamage(uint32_t seq, uint32_t targetId, int32_t damage, uint8_t dmgType) {
	return "{\"kind\":\"event\",\"type\":\"damage\",\"seq\":" + u32s(seq) + ",\"target_id\":" + u32s(targetId) +
	       ",\"damage\":" + i32s(damage) + ",\"dmg_type\":" + u8s(dmgType) + tsField() + "}";
}

inline std::string BuildDeath(uint32_t targetId) {
	return "{\"kind\":\"event\",\"type\":\"death\",\"target_id\":" + u32s(targetId) + tsField() + "}";
}

inline std::string BuildInventory(uint32_t containerId, uint32_t itemId, uint16_t qty, bool deleted) {
	return "{\"kind\":\"event\",\"type\":\"inventory\",\"container_id\":" + u32s(containerId) +
	       ",\"item_id\":" + u32s(itemId) + ",\"qty\":" + u16s(qty) +
	       ",\"deleted\":" + bools(deleted) + tsField() + "}";
}

inline std::string BuildDialog(uint32_t nodeId, bool close) {
	return "{\"kind\":\"event\",\"type\":\"dialog\",\"node_id\":" + u32s(nodeId) +
	       ",\"close\":" + bools(close) + tsField() + "}";
}

inline std::string BuildWorldFlag(const std::string &key, bool value) {
	return "{\"kind\":\"event\",\"type\":\"flag\",\"key\":\"" + esc(key) +
	       "\",\"value\":" + bools(value) + tsField() + "}";
}

inline std::string BuildAiDelta(uint32_t entityId, int16_t x, int16_t y, uint8_t floor, uint8_t state) {
	return "{\"kind\":\"event\",\"type\":\"ai_delta\",\"entity_id\":" + u32s(entityId) +
	       ",\"x\":" + std::to_string(static_cast<int>(x)) +
	       ",\"y\":" + std::to_string(static_cast<int>(y)) +
	       ",\"floor\":" + u8s(floor) + ",\"state\":" + u8s(state) + tsField() + "}";
}

inline std::string BuildBossPhase(uint32_t entityId, uint8_t phase, int32_t hp) {
	return "{\"kind\":\"event\",\"type\":\"boss\",\"entity_id\":" + u32s(entityId) +
	       ",\"phase\":" + u8s(phase) + ",\"hp\":" + i32s(hp) + tsField() + "}";
}

inline std::string BuildChat(uint32_t seq, uint16_t senderId, const std::string &msg) {
	return "{\"kind\":\"event\",\"type\":\"chat\",\"seq\":" + u32s(seq) + ",\"sender_id\":" + u16s(senderId) +
	       ",\"msg\":\"" + esc(msg) + "\"" + tsField() + "}";
}

inline std::string BuildWorldLight(uint8_t level, uint8_t color) {
	return "{\"kind\":\"event\",\"type\":\"light\",\"level\":" + u8s(level) +
	       ",\"color\":" + u8s(color) + tsField() + "}";
}

inline std::string BuildWeather(uint8_t wtype, uint16_t intensity) {
	return "{\"kind\":\"event\",\"type\":\"weather\",\"wtype\":" + u8s(wtype) +
	       ",\"intensity\":" + u16s(intensity) + tsField() + "}";
}

inline std::string BuildSound(uint16_t soundId, uint16_t x, uint16_t y, uint8_t z, uint8_t vol) {
	return "{\"kind\":\"event\",\"type\":\"sound\",\"sound_id\":" + u16s(soundId) +
	       ",\"x\":" + u16s(x) + ",\"y\":" + u16s(y) +
	       ",\"z\":" + u8s(z) + ",\"vol\":" + u8s(vol) + tsField() + "}";
}

inline std::string BuildEntityState(uint16_t uniqueId, uint8_t flags, float light, uint32_t expiresMs) {
	return "{\"kind\":\"event\",\"type\":\"entity_state\",\"unique_id\":" + u16s(uniqueId) +
	       ",\"flags\":" + u8s(flags) + ",\"light\":" + fs(light) +
	       ",\"expires_ms\":" + u32s(expiresMs) + tsField() + "}";
}

inline std::string BuildMapUpdate(uint16_t x, uint16_t y, uint8_t z, uint16_t itemId) {
	return "{\"kind\":\"event\",\"type\":\"map_update\",\"x\":" + u16s(x) +
	       ",\"y\":" + u16s(y) + ",\"z\":" + u8s(z) +
	       ",\"item_id\":" + u16s(itemId) + tsField() + "}";
}

inline std::string BuildPresence(uint32_t seq, uint16_t userId, uint16_t x, uint16_t y, uint8_t z) {
	return "{\"kind\":\"event\",\"type\":\"presence\",\"seq\":" + u32s(seq) + ",\"user_id\":" + u16s(userId) +
	       ",\"x\":" + u16s(x) + ",\"y\":" + u16s(y) +
	       ",\"z\":" + u8s(z) + tsField() + "}";
}

inline std::string BuildLock(uint16_t userId, uint16_t x, uint16_t y, uint8_t z, uint8_t mode, uint16_t leaseMs) {
	return "{\"kind\":\"event\",\"type\":\"lock\",\"user_id\":" + u16s(userId) +
	       ",\"x\":" + u16s(x) + ",\"y\":" + u16s(y) +
	       ",\"z\":" + u8s(z) + ",\"mode\":" + u8s(mode) +
	       ",\"lease_ms\":" + u16s(leaseMs) + tsField() + "}";
}

/** Elemento de array tiles: {"x":N,"y":N,"z":N,"shape":"...","type":"...","feature":"...","item_id":N} */
inline std::string BuildTile(int x, int y, int z,
                              const std::string &shape,
                              const std::string &type,
                              const std::string &feature,
                              uint32_t itemId) {
	return "{\"x\":"         + std::to_string(x) +
	       ",\"y\":"         + std::to_string(y) +
	       ",\"z\":"         + std::to_string(z) +
	       ",\"shape\":\""   + shape   + "\""
	       ",\"type\":\""    + type    + "\""
	       ",\"feature\":\"" + feature + "\""
	       ",\"item_id\":"   + u32s(itemId) + "}";
}

/** {"kind":"chunk","chunk_x":N,"chunk_y":N,"seq":N,"player":{"x":N,"y":N,"z":N},"tiles":[...]} */
inline std::string BuildChunk(uint16_t chunkX, uint16_t chunkY, uint32_t seq,
                               int playerX, int playerY, int playerZ,
                               const std::string &tilesJson) {
	return "{\"kind\":\"chunk\""
	       ",\"chunk_x\":"  + u16s(chunkX) +
	       ",\"chunk_y\":"  + u16s(chunkY) +
	       ",\"seq\":"      + u32s(seq) +
	       ",\"player\":{\"x\":" + std::to_string(playerX) +
	                    ",\"y\":" + std::to_string(playerY) +
	                    ",\"z\":" + std::to_string(playerZ) + "}"
	       ",\"tiles\":["   + tilesJson + "]" + tsField() + "}";
}

inline std::string BuildHeartbeat(uint32_t seq) {
	return "{\"kind\":\"heartbeat\",\"seq\":" + u32s(seq) + tsField() + "}";
}

inline std::string BuildFragment(uint32_t id, uint16_t index, uint16_t count, const std::string &data) {
	return "{\"kind\":\"fragment\",\"id\":" + u32s(id) +
	       ",\"index\":" + u16s(index) + ",\"count\":" + u16s(count) +
	       ",\"encoding\":\"json-split\",\"data\":\"" + esc(data) + "\"" + tsField() + "}";
}

} // namespace mv3d::utp
