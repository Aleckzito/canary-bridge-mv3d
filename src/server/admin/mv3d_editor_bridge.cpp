/**
 * MV3D editor bridge - raw local TCP bridge for MV3D Hybrid Engine.
 */

#include "server/admin/mv3d_editor_bridge.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

// BLOG: escribe trafico de paquetes solo al archivo de log (nunca a consola).
// warn/error siguen usando logger.warn/error para aparecer en ambos lugares.
#define BLOG(...) do { if (bridgeFileLogger_) { bridgeFileLogger_->info(__VA_ARGS__); } } while (false)

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "config/configmanager.hpp"
#include "creatures/players/player.hpp"
#include "creatures/npcs/npc.hpp"
#include "game/game.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "items/item.hpp"
#include "lib/logging/logger.hpp"
#include "security/rsa.hpp"
#include "server/admin/mv3d_utp_event_builder.hpp"

#include <openssl/rand.h>

#include <filesystem>
#include <sstream>

namespace {
std::atomic_bool g_isMapSaving { false };
constexpr size_t UTP_SAFE_MTU = 1400;
constexpr size_t UTP_FRAGMENT_PAYLOAD_SIZE = 900;
constexpr auto UTP_HEARTBEAT_INTERVAL = std::chrono::seconds(1);
constexpr auto UTP_SUBSCRIBER_TTL = std::chrono::seconds(10);

// ── UTP Chunk Catalog — itemId → (shape, type, feature) ──────────────────────
// Fuente: utp_core/UTP_CATALOG.md  Expandir con derivación OTB/DAT cuando esté listo.
struct CatalogEntry { const char* shape; const char* type; const char* feature; };

const CatalogEntry* utpCatalogLookup(uint16_t itemId) {
    static const std::unordered_map<uint16_t, CatalogEntry> kCatalog = {
        {   1, {"floor",           "water",      ""}           },
        { 100, {"floor",           "void",       "hueco"}      },
        { 103, {"floor",           "dirt",       ""}           },
        { 104, {"floor",           "sand",       ""}           },
        { 106, {"floor",           "grass",      ""}           },
        { 116, {"prop_neutral",    "wood",       ""}           },
        { 194, {"floor",           "dirt",       ""}           },
        { 231, {"floor",           "sand",       ""}           },
        { 293, {"floor",           "grass",      ""}           },
        { 305, {"floor",           "dirt",       ""}           },
        { 351, {"floor",           "dirt",       ""}           },
        { 356, {"wall_horizontal", "stone_wall", "pared"}      },
        { 373, {"wall_vertical",   "stone_wall", "pared"}      },
        { 385, {"floor",           "void",       "hueco"}      },
        { 394, {"floor",           "void",       "hueco"}      },
        { 413, {"stairs",          "wood",       "escalera"}   },
        { 428, {"stairs",          "stone_wall", "escalera"}   },
        { 433, {"ramp",            "ladder",     "escalera"}   },
        { 482, {"ramp",            "ladder",     "escalera"}   },
        { 594, {"floor",           "void",       "hueco"}      },
        { 622, {"floor",           "water",      ""}           },
        { 727, {"floor",           "lava",       ""}           },
        { 746, {"prop_neutral",    "stone_wall", ""}           },
        { 920, {"wall_vertical",   "stone_wall", "pared"}      },
        { 958, {"vegetation",      "wood",       "tall_foliage"}},
        {1270, {"wall_horizontal", "brick_wall", "pared"}      },
        {1922, {"prop_neutral",    "stone_wall", ""}           },
        {2012, {"prop_neutral",    "wood",       ""}           },
        {2025, {"prop_neutral",    "stone_wall", ""}           },
        {2322, {"prop_neutral",    "wood",       ""}           },
        {2519, {"prop_neutral",    "wood",       ""}           },
        {3639, {"vegetation",      "wood",       "foliage"}    },
        {3681, {"vegetation",      "wood",       "foliage"}    },
    };
    auto it = kCatalog.find(itemId);
    return it != kCatalog.end() ? &it->second : nullptr;
}

std::string endpointKey(const asio::ip::udp::endpoint &ep) {
	return ep.address().to_string() + ":" + std::to_string(ep.port());
}

template <size_t N>
uint16_t readU16LE(const std::array<uint8_t, N> &buffer, const size_t offset) {
	return static_cast<uint16_t>(buffer[offset] | (buffer[offset + 1] << 8));
}

template <size_t N>
uint32_t readU32LE(const std::array<uint8_t, N> &buffer, const size_t offset) {
	return static_cast<uint32_t>(buffer[offset]) | (static_cast<uint32_t>(buffer[offset + 1]) << 8) | (static_cast<uint32_t>(buffer[offset + 2]) << 16) | (static_cast<uint32_t>(buffer[offset + 3]) << 24);
}

// JSON Parsing Helpers for UTP Commands
[[nodiscard]] size_t KeyPos(std::string_view json, std::string_view key) {
	std::string needle = "\""; needle += key; needle += "\":";
	auto pos = json.find(needle);
	if (pos == std::string_view::npos) return std::string_view::npos;
	pos += needle.size();
	while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
	return pos;
}

[[nodiscard]] std::string_view FieldStr(std::string_view json, std::string_view key) {
	auto pos = KeyPos(json, key);
	if (pos == std::string_view::npos || pos >= json.size() || json[pos] != '"') return {};
	++pos;
	auto end = json.find('"', pos);
	return (end == std::string_view::npos) ? std::string_view{} : json.substr(pos, end - pos);
}

[[nodiscard]] int32_t FieldInt(std::string_view json, std::string_view key, int32_t fallback = 0) {
	auto pos = KeyPos(json, key);
	if (pos == std::string_view::npos) return fallback;
	try {
		return std::stoi(std::string(json.substr(pos, json.find_first_of(",}", pos) - pos)));
	} catch (...) { return fallback; }
}

template <size_t N>
void writeU16LE(std::array<uint8_t, N> &buffer, const size_t offset, const uint16_t value) {
	buffer[offset] = static_cast<uint8_t>(value & 0xFF);
	buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void writeU16LE(std::vector<uint8_t> &buffer, const size_t offset, const uint16_t value) {
	buffer[offset] = static_cast<uint8_t>(value & 0xFF);
	buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

std::shared_ptr<Item> resolveTileItemForEdit(const std::shared_ptr<Tile> &tile, const uint8_t stackIndex, const uint16_t expectedItemId, int32_t &resolvedStackIndex) {
	resolvedStackIndex = -1;
	if (!tile) {
		return nullptr;
	}

	auto matchesExpectedItemId = [expectedItemId](const std::shared_ptr<Item> &item) {
		return item && (expectedItemId == 0 || item->getID() == expectedItemId);
	};

	if (const auto thingAtStack = tile->getThing(stackIndex)) {
		if (const auto itemAtStack = thingAtStack->getItem(); matchesExpectedItemId(itemAtStack)) {
			resolvedStackIndex = tile->getThingIndex(itemAtStack);
			return itemAtStack;
		}
	}

	if (const auto useItem = tile->getUseItem(static_cast<int32_t>(stackIndex)); matchesExpectedItemId(useItem)) {
		resolvedStackIndex = tile->getThingIndex(useItem);
		return useItem;
	}

	if (expectedItemId != 0) {
		if (const auto* items = tile->getItemList(); items && !items->empty()) {
			for (auto it = items->rbegin(); it != items->rend(); ++it) {
				if ((*it) && (*it)->getID() == expectedItemId) {
					resolvedStackIndex = tile->getThingIndex(*it);
					return *it;
				}
			}
		}

		if (const auto ground = tile->getGround(); ground && ground->getID() == expectedItemId) {
			resolvedStackIndex = tile->getThingIndex(ground);
			return ground;
		}
	}

	return nullptr;
}

std::optional<uint16_t> parseSubscribePort(const std::string_view json) {
	if (json.find("\"subscribe\"") == std::string_view::npos) {
		return std::nullopt;
	}

	const auto portKey = json.find("\"port\"");
	if (portKey == std::string_view::npos) {
		return static_cast<uint16_t>(4242);
	}

	const auto colon = json.find(':', portKey);
	if (colon == std::string_view::npos) {
		return std::nullopt;
	}

	size_t pos = colon + 1;
	while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '"')) {
		++pos;
	}

	unsigned long value = 0;
	bool hasDigit = false;
	while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
		hasDigit = true;
		value = value * 10 + static_cast<unsigned long>(json[pos] - '0');
		if (value > 65535) {
			return std::nullopt;
		}
		++pos;
	}

	if (!hasDigit || value == 0) {
		return std::nullopt;
	}

	return static_cast<uint16_t>(value);
}
} // namespace

MV3DEditorBridge::MV3DEditorBridge(Logger &logger) :
	logger(logger) {
	if (const char* customPort = std::getenv("MV3D_EDITOR_BRIDGE_PORT")) {
		const auto parsed = std::atoi(customPort);
		if (parsed > 0 && parsed <= 65535) {
			port = static_cast<uint16_t>(parsed);
		}
	}

	if (const char* playerId = std::getenv("MV3D_PLAYER_ID")) {
		const auto parsed = std::strtoul(playerId, nullptr, 10);
		if (parsed <= std::numeric_limits<uint32_t>::max()) {
			mv3dPlayerId = static_cast<uint32_t>(parsed);
		}
	}

	if (const char* customUtpPort = std::getenv("MV3D_UTP_PORT")) {
		const auto parsed = std::atoi(customUtpPort);
		if (parsed > 0 && parsed <= 65535) {
			utpPort = static_cast<uint16_t>(parsed);
		}
	}

	if (const char* customSubscribePort = std::getenv("MV3D_UTP_SUBSCRIBE_PORT")) {
		const auto parsed = std::atoi(customSubscribePort);
		if (parsed > 0 && parsed <= 65535) {
			utpSubscribePort = static_cast<uint16_t>(parsed);
		}
	}

	if (const char* customQueue = std::getenv("MV3D_UTP_MAX_QUEUE")) {
		const auto parsed = std::atoi(customQueue);
		if (parsed > 0) {
			utpMaxQueue = static_cast<size_t>(parsed);
		}
	}
}

MV3DEditorBridge::~MV3DEditorBridge() {
	stop();
}

bool MV3DEditorBridge::isMapEditOpcode(const uint8_t opcode) const {
	switch (opcode) {
		case C2S_MAP_EDIT_PAINT:
		case C2S_MAP_EDIT_ERASE:
		case C2S_MAP_SYNC_REQUEST:
		case C2S_ERASE_BY_STACK:
		case C2S_MAP_SAVE_REQUEST:
		case C2S_ITEM_UPDATE_METADATA:
		case C2S_ITEM_UPDATE_TEXT:
		case C2S_LOCK_REQUEST:
		case C2S_USER_PRESENCE_UPDATE:
		case C2S_ENTITY_TRIGGER:
			return true;
		default:
			return false;
	}
}

bool MV3DEditorBridge::isPlayerOperationOpcode(const uint8_t opcode) const {
	switch (opcode) {
		case C2S_ATTACK_TARGET:
		case C2S_ITEM_USE_REQUEST:
		case C2S_DIALOG_CHOICE:
		case C2S_AI_BEHAVIOR_REPORT:
		case C2S_BOSS_PHASE_REPORT:
		case C2S_CHAT_MESSAGE:
		case C2S_LISTENER_UPDATE:
			return true;
		default:
			return false;
	}
}

void MV3DEditorBridge::start() {
	enabled = g_configManager().getBoolean(MV3D_EDITOR_BRIDGE_ENABLED);
	mapEditMode = g_configManager().getBoolean(MV3D_EDITOR_BRIDGE_MAP_EDIT_MODE);
	playerOperationMode = g_configManager().getBoolean(MV3D_EDITOR_BRIDGE_PLAYER_OPERATION_MODE);

	if (const char* disabled = std::getenv("MV3D_EDITOR_BRIDGE_DISABLED")) {
		enabled = std::string_view(disabled) != "1";
	}

	if (!playerOperationMode) {
		mv3dPlayerId = 0;
	}

	if (!enabled || running.exchange(true)) {
		return;
	}

	try {
		std::filesystem::create_directories("logs");
		bridgeFileLogger_ = spdlog::rotating_logger_mt(
			"mv3d_bridge",
			"logs/mv3d_bridge.log",
			5 * 1024 * 1024, // 5 MB por archivo
			3                // 3 rotaciones
		);
		bridgeFileLogger_->set_level(spdlog::level::debug);
		bridgeFileLogger_->flush_on(spdlog::level::warn);
	} catch (const spdlog::spdlog_ex &ex) {
		logger.warn("MV3D bridge: no se pudo abrir logs/mv3d_bridge.log: {}", ex.what());
	}

	try {
		ioContext = std::make_unique<asio::io_context>();
		acceptor = std::make_unique<asio::ip::tcp::acceptor>(
			*ioContext,
			asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port)
		);
		acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true));

		utpSocket = std::make_unique<asio::ip::udp::socket>(
			*ioContext,
			asio::ip::udp::endpoint(asio::ip::udp::v4(), 0)
		);
		registerUTPSubscriber(asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), utpPort));

		utpSubscribeSocket = std::make_unique<asio::ip::udp::socket>(
			*ioContext,
			asio::ip::udp::endpoint(asio::ip::udp::v4(), utpSubscribePort)
		);

		if (const char* target = std::getenv("MV3D_UTP_TARGET")) {
			const std::string targetString(target);
			const auto colon = targetString.rfind(':');
			if (colon != std::string::npos) {
				try {
					const auto host = targetString.substr(0, colon);
					const auto parsedPort = static_cast<unsigned long>(std::stoul(targetString.substr(colon + 1)));
					if (parsedPort > 0 && parsedPort <= 65535) {
						registerUTPSubscriber(asio::ip::udp::endpoint(
							asio::ip::make_address(host),
							static_cast<uint16_t>(parsedPort)
						));
					}
				} catch (const std::exception &ex) {
					logger.warn("MV3D bridge ignored invalid MV3D_UTP_TARGET='{}': {}", targetString, ex.what());
				}
			}
		}

		worker = std::thread(&MV3DEditorBridge::runLoop, this);
		utpOutboundWorker = std::thread(&MV3DEditorBridge::runUTPOutboundLoop, this);
		utpSubscriberWorker = std::thread(&MV3DEditorBridge::runUTPSubscriberLoop, this);
		utpHeartbeatWorker = std::thread(&MV3DEditorBridge::runUTPHeartbeatLoop, this);
		logger.info("MV3D editor bridge listening on tcp://127.0.0.1:{}", port);
		logger.info("MV3D bridge modes: map_edit={}, player_operation={}, mv3d_player_id={}",
			mapEditMode ? "enabled" : "disabled",
			playerOperationMode ? "enabled" : "disabled",
			mv3dPlayerId);
		logger.info("MV3D bridge UTP ready on udp://127.0.0.1:{}", utpPort);
		logger.info("MV3D bridge UTP subscription listening on udp://0.0.0.0:{}", utpSubscribePort);
		g_dispatcher().addEvent(
			[this] { loadEditsFromDisk(); },
			"MV3DEditorBridge::loadEditsFromDisk"
		);
	} catch (const std::exception &ex) {
		running = false;
		utpSubscribeSocket.reset();
		utpSocket.reset();
		ioContext.reset();
		acceptor.reset();
		logger.error("MV3D editor bridge could not start: {}", ex.what());
	}
}

void MV3DEditorBridge::stop() {
	if (!running.exchange(false)) {
		return;
	}

	utpQueueCv.notify_all();

	{
		std::scoped_lock lock(socketMutex);
		for (auto &client : clients) {
			if (client && client->is_open()) {
				asio::error_code ec;
				client->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
				client->close(ec);
			}
		}
		clients.clear();
	}

	if (acceptor) {
		asio::error_code ec;
		acceptor->close(ec);
	}

	if (utpSocket) {
		asio::error_code ec;
		utpSocket->close(ec);
	}

	if (utpSubscribeSocket) {
		asio::error_code ec;
		utpSubscribeSocket->close(ec);
	}

	if (ioContext) {
		ioContext->stop();
	}

	if (worker.joinable()) {
		worker.join();
	}

	if (utpOutboundWorker.joinable()) {
		utpOutboundWorker.join();
	}

	if (utpSubscriberWorker.joinable()) {
		utpSubscriberWorker.join();
	}

	if (utpHeartbeatWorker.joinable()) {
		utpHeartbeatWorker.join();
	}

	acceptor.reset();
	utpSocket.reset();
	utpSubscribeSocket.reset();
	utpHighPriorityQueue.clear();
	utpNormalPriorityQueue.clear();
	ioContext.reset();

	if (bridgeFileLogger_) {
		bridgeFileLogger_->flush();
		spdlog::drop("mv3d_bridge");
		bridgeFileLogger_.reset();
	}
}

void MV3DEditorBridge::runLoop() {
	while (running) {
		auto client = std::make_shared<asio::ip::tcp::socket>(*ioContext);
		asio::error_code ec;
		acceptor->accept(*client, ec);
		if (ec) {
			if (running) {
				logger.warn("MV3D editor bridge accept failed: {}", ec.message());
			}
			continue;
		}

		{
			std::scoped_lock lock(socketMutex);
			clients.push_back(client);
			socketToUserId[client] = nextUserId++;
		}

		logger.info("MV3D editor bridge client connected (total={}, assigned userId={})", clients.size(), socketToUserId[client]);

		// Manejar cada cliente de forma independiente
		std::thread([this, client] { handleClient(client); }).detach();
	}
}

void MV3DEditorBridge::runUTPOutboundLoop() {
	while (true) {
		std::string json;
		{
			std::unique_lock lock(utpQueueMutex);
			utpQueueCv.wait(lock, [this] {
				return !running || !utpHighPriorityQueue.empty() || !utpNormalPriorityQueue.empty();
			});

			if (utpHighPriorityQueue.empty() && utpNormalPriorityQueue.empty()) {
				if (!running) {
					break;
				}
				continue;
			}

			if (!utpHighPriorityQueue.empty()) {
				json = std::move(utpHighPriorityQueue.front());
				utpHighPriorityQueue.pop_front();
			} else {
				json = std::move(utpNormalPriorityQueue.front());
				utpNormalPriorityQueue.pop_front();
			}
		}

		std::vector<asio::ip::udp::endpoint> subscribers;
		{
			std::scoped_lock lock(utpMutex);
			subscribers.reserve(utpSubscribers.size());
			for (const auto &[key, sub] : utpSubscribers) {
				subscribers.push_back(sub.endpoint);
			}
		}

		if (!utpSocket || !utpSocket->is_open()) {
			continue;
		}

		for (const auto &ep : subscribers) {
			asio::error_code ec;
			utpSocket->send_to(asio::buffer(json.data(), json.size()), ep, 0, ec);
			if (ec) {
				// No usar ep.address().to_string() aqui por si acaso la IP es la que crashea
				BLOG("[UTP] Fallo de envio a endpoint: {}", ec.message());
			}
		}
	}
}

void MV3DEditorBridge::runUTPHeartbeatLoop() {
	while (running) {
		std::this_thread::sleep_for(UTP_HEARTBEAT_INTERVAL);
		if (!running) {
			break;
		}

		pruneUTPSubscribers();
		broadcastUTP(mv3d::utp::BuildHeartbeat(++utpHeartbeatSeq));
	}
}

void MV3DEditorBridge::runUTPSubscriberLoop() {
	if (!utpSubscribeSocket) {
		return;
	}

	std::array<char, 512> buffer {};
	while (running) {
		asio::ip::udp::endpoint remote;
		asio::error_code ec;
		const auto bytes = utpSubscribeSocket->receive_from(asio::buffer(buffer), remote, 0, ec);
		if (ec) {
			if (running && ec != asio::error::operation_aborted && ec != asio::error::bad_descriptor) {
				logger.warn("MV3D bridge UTP subscription receive failed: {}", ec.message());
			}
			continue;
		}

		const std::string_view packet(buffer.data(), bytes);
		const auto requestedPort = parseSubscribePort(packet);
		if (requestedPort) {
			const auto addr = remote.address();
			const uint16_t port = *requestedPort;
			
			BLOG("MV3D bridge UTP attempting registration for {}:{}", addr.to_string(), port);
			
			asio::ip::udp::endpoint subscriber(addr, port);
			registerUTPSubscriber(subscriber);
			
			BLOG("MV3D bridge UTP registration SUCCESS for {}:{}", addr.to_string(), port);
			continue;
		}

		// Si no es suscripción, intentar parsear como COMANDO UTP (Inbound)
		const auto kind = FieldStr(packet, "kind");
		if (kind == "event") {
			const auto type = FieldStr(packet, "type");
			if (type == "chat") {
				ChatMessageCommand cmd;
				cmd.message = std::string(FieldStr(packet, "msg"));
				g_dispatcher().addEvent([this, cmd = std::move(cmd)] {
					broadcastChatMessage(0, cmd.message); // 0 = System/Bridge relay
				}, "MV3D_UDP_Chat");
			} else if (type == "presence") {
				PresenceUpdate cmd;
				cmd.x = static_cast<uint16_t>(FieldInt(packet, "x"));
				cmd.y = static_cast<uint16_t>(FieldInt(packet, "y"));
				cmd.z = static_cast<uint8_t>(FieldInt(packet, "z"));
				g_dispatcher().addEvent([this, cmd] {
					applyPresenceUpdate(std::weak_ptr<asio::ip::tcp::socket>(), cmd);
				}, "MV3D_UDP_Presence");
			}
		}
	}
}

bool MV3DEditorBridge::performBridgeHandshake(const std::shared_ptr<asio::ip::tcp::socket> &client) {
	// Pre-protocol magic bytes (used only during handshake, before the opcode loop)
	constexpr uint8_t HS_HELLO = 0xAA;
	constexpr uint8_t HS_OK    = 0xBB;
	constexpr uint8_t HS_FAIL  = 0xCC;

	// Set 5-second receive/send timeout for the handshake window
	auto setSocketTimeout = [&](int ms) {
#ifdef _WIN32
		DWORD tv = static_cast<DWORD>(ms);
		setsockopt(client->native_handle(), SOL_SOCKET, SO_RCVTIMEO,
		           reinterpret_cast<const char*>(&tv), sizeof(tv));
		setsockopt(client->native_handle(), SOL_SOCKET, SO_SNDTIMEO,
		           reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
		struct timeval tv { ms / 1000, (ms % 1000) * 1000 };
		setsockopt(client->native_handle(), SOL_SOCKET, SO_RCVTIMEO,
		           reinterpret_cast<const char*>(&tv), sizeof(tv));
		setsockopt(client->native_handle(), SOL_SOCKET, SO_SNDTIMEO,
		           reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
	};

	setSocketTimeout(5000);

	asio::error_code ec;

	// ── Step 1: Server challenges client ──────────────────────────────────────
	// Generate 16-byte nonce and send HS_HELLO + nonce (17 bytes total)
	std::array<uint8_t, 16> serverNonce {};
	if (RAND_bytes(serverNonce.data(), 16) != 1) {
		logger.error("MV3D bridge handshake: RAND_bytes failed");
		setSocketTimeout(0);
		return false;
	}

	std::array<uint8_t, 17> hello {};
	hello[0] = HS_HELLO;
	std::memcpy(hello.data() + 1, serverNonce.data(), 16);
	asio::write(*client, asio::buffer(hello), ec);
	if (ec) {
		BLOG("MV3D bridge handshake: send HELLO failed: {}", ec.message());
		setSocketTimeout(0);
		return false;
	}

	// Read 128-byte client response: client encrypted the padded nonce with the server's public key
	std::array<uint8_t, 128> response {};
	asio::read(*client, asio::buffer(response), ec);
	if (ec) {
		logger.warn("MV3D bridge handshake: read client response failed (timeout?): {}", ec.message());
		setSocketTimeout(0);
		return false;
	}

	// Decrypt: m = c^d mod n — nonce must be at bytes [112..127]
	char decryptBuf[128];
	std::memcpy(decryptBuf, response.data(), 128);
	g_RSA().decrypt(decryptBuf);
	if (std::memcmp(decryptBuf + 112, serverNonce.data(), 16) != 0) {
		logger.warn("MV3D bridge handshake: client nonce verification FAILED — rejecting");
		std::array<uint8_t, 1> fail { HS_FAIL };
		asio::write(*client, asio::buffer(fail), ec);
		setSocketTimeout(0);
		return false;
	}

	std::array<uint8_t, 1> ok { HS_OK };
	asio::write(*client, asio::buffer(ok), ec);
	if (ec) {
		BLOG("MV3D bridge handshake: send AUTH_OK (step 1) failed: {}", ec.message());
		setSocketTimeout(0);
		return false;
	}

	// ── Step 2: Client challenges server ──────────────────────────────────────
	// Read 16-byte client nonce
	std::array<uint8_t, 16> clientNonce {};
	asio::read(*client, asio::buffer(clientNonce), ec);
	if (ec) {
		logger.warn("MV3D bridge handshake: read client_nonce failed: {}", ec.message());
		setSocketTimeout(0);
		return false;
	}

	// Build padded block, sign with private key: s = m^d mod n
	char signBuf[128] = {};
	std::memcpy(signBuf + 112, clientNonce.data(), 16);
	g_RSA().decrypt(signBuf);

	asio::write(*client, asio::buffer(signBuf, 128), ec);
	if (ec) {
		BLOG("MV3D bridge handshake: send signed nonce failed: {}", ec.message());
		setSocketTimeout(0);
		return false;
	}

	// Read client confirmation
	std::array<uint8_t, 1> confirm {};
	asio::read(*client, asio::buffer(confirm), ec);
	setSocketTimeout(0);
	if (ec) {
		logger.warn("MV3D bridge handshake: read client confirm failed: {}", ec.message());
		return false;
	}

	if (confirm[0] != HS_OK) {
		logger.warn("MV3D bridge handshake: client rejected server signature (0x{:02X})", confirm[0]);
		return false;
	}

	BLOG("MV3D bridge handshake: mutual RSA-1024 auth OK");
	return true;
}

void MV3DEditorBridge::handleClient(std::shared_ptr<asio::ip::tcp::socket> client) {
	if (!performBridgeHandshake(client)) {
		asio::error_code closeEc;
		client->close(closeEc);
		return;
	}

	asio::error_code ec;
	while (running && client->is_open()) {
		std::array<uint8_t, 1> opcodeBuffer {};
		asio::read(*client, asio::buffer(opcodeBuffer), ec);
		if (ec) {
			break;
		}

		const uint8_t opcode = opcodeBuffer[0];
		if (isMapEditOpcode(opcode) && !mapEditMode) {
			logger.warn("MV3D bridge rejected map/edit opcode 0x{:02X}: mv3dBridgeMapEditMode=false", static_cast<int>(opcode));
			break;
		}
		if (isPlayerOperationOpcode(opcode) && !playerOperationMode) {
			logger.warn("MV3D bridge rejected player opcode 0x{:02X}: mv3dBridgePlayerOperationMode=false", static_cast<int>(opcode));
			break;
		}

			if (opcode == C2S_MAP_EDIT_PAINT) {
				std::array<uint8_t, PAINT_PAYLOAD_SIZE> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0x90: {}", ec.message());
					}
					break;
				}

				handlePaintPacket(client, payload);
			} else if (opcode == C2S_MAP_EDIT_ERASE) {
				std::array<uint8_t, ERASE_PAYLOAD_SIZE> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0x91: {}", ec.message());
					}
					break;
				}

				handleErasePacket(client, payload);
			} else if (opcode == C2S_ERASE_BY_STACK) {
				std::array<uint8_t, ERASE_BY_STACK_PAYLOAD_SIZE> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0x95: {}", ec.message());
					}
					break;
				}

				handleEraseByStackPacket(client, payload);
			} else if (opcode == C2S_MAP_SYNC_REQUEST) {
				std::array<uint8_t, SYNC_REQUEST_PAYLOAD_SIZE> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0x93: {}", ec.message());
					}
					break;
				}

				handleSyncRequestPacket(client, payload);
			} else if (opcode == C2S_MAP_SAVE_REQUEST) {
				handleSaveRequestPacket(client);
			} else if (opcode == C2S_ITEM_UPDATE_METADATA) {
				std::array<uint8_t, METADATA_PAYLOAD_SIZE> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0x98: {}", ec.message());
					}
					break;
				}

				handleMetadataUpdatePacket(client, payload);
			} else if (opcode == C2S_ITEM_UPDATE_TEXT) {
				std::array<uint8_t, TEXT_HEADER_PAYLOAD_SIZE> header {};
				asio::read(*client, asio::buffer(header), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0x99 header: {}", ec.message());
					}
					break;
				}

				TextUpdateCommand command;
				command.x = readU16LE(header, 0);
				command.y = readU16LE(header, 2);
				command.z = header[4];
				command.itemId = readU16LE(header, 5);
				command.stackIndex = header[7];
				command.transactionId = readU32LE(header, 8);

				std::array<uint8_t, 2> textLenBytes {};
				asio::read(*client, asio::buffer(textLenBytes), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0x99 len: {}", ec.message());
					}
					break;
				}

				const uint16_t textLen = static_cast<uint16_t>(textLenBytes[0] | (textLenBytes[1] << 8));
				if (textLen > 4096) {
					logger.warn("MV3D bridge 0x99 textLen {} excede limite, cerrando conexion", textLen);
					break;
				}
				if (textLen > 0) {
					std::vector<uint8_t> textBuffer(textLen);
					asio::read(*client, asio::buffer(textBuffer), ec);
					if (ec) {
						if (running) {
							BLOG("MV3D bridge client drop while reading 0x99 text: {}", ec.message());
						}
						break;
					}
					command.text.assign(textBuffer.begin(), textBuffer.end());
				}

				handleTextUpdatePacket(client, std::move(command));
			} else if (opcode == C2S_ATTACK_TARGET) {
				std::array<uint8_t, ATTACK_PAYLOAD_SIZE> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0xB0: {}", ec.message());
					}
					break;
				}
				handleAttackPacket(client, payload);
			} else if (opcode == C2S_ITEM_USE_REQUEST) {
				std::array<uint8_t, ITEM_USE_PAYLOAD_SIZE> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0xB3: {}", ec.message());
					}
					break;
				}
				handleItemUsePacket(client, payload);
			} else if (opcode == C2S_DIALOG_CHOICE) {
				std::array<uint8_t, DIALOG_CHOICE_PAYLOAD_SIZE> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0xB6: {}", ec.message());
					}
					break;
				}
				handleDialogChoicePacket(client, payload);
			} else if (opcode == C2S_AI_BEHAVIOR_REPORT) {
				std::array<uint8_t, 10> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0xB9: {}", ec.message());
					}
					break;
				}
				handleAiBehaviorReportPacket(client, payload);
			} else if (opcode == C2S_BOSS_PHASE_REPORT) {
				std::array<uint8_t, BOSS_REPORT_PAYLOAD_SIZE> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0xBB: {}", ec.message());
					}
					break;
				}
				handleBossPhaseReportPacket(client, payload);
			} else if (opcode == C2S_LOCK_REQUEST) {
				std::array<uint8_t, LOCK_REQUEST_PAYLOAD_SIZE> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0x9B: {}", ec.message());
					}
					break;
				}
				handleLockRequestPacket(client, payload);
			} else if (opcode == C2S_USER_PRESENCE_UPDATE) {
				std::array<uint8_t, PRESENCE_UPDATE_PAYLOAD_SIZE> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0x9E: {}", ec.message());
					}
					break;
				}
				handlePresenceUpdatePacket(client, payload);
			} else if (opcode == C2S_ENTITY_TRIGGER) {
				std::array<uint8_t, ENTITY_TRIGGER_PAYLOAD_SIZE> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (ec) {
					if (running) {
						BLOG("MV3D bridge client drop while reading 0xAC: {}", ec.message());
					}
					break;
				}
				handleEntityTriggerPacket(client, payload);
			} else if (opcode == C2S_CHAT_MESSAGE) {
				handleChatMessagePacket(client);
			} else if (opcode == C2S_LISTENER_UPDATE) {
				std::array<uint8_t, 24> payload {};
				asio::read(*client, asio::buffer(payload), ec);
				if (!ec) {
					handleListenerUpdatePacket(client, payload);
				}
			} else {
				logger.warn("MV3D editor bridge ignored opcode 0x{:02X}", static_cast<int>(opcode));
			}
		}

		{
			std::scoped_lock lock(socketMutex);
			socketToUserId.erase(client);
			clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
		}
		logger.info("MV3D editor bridge client disconnected (total={})", clients.size());
	}

void MV3DEditorBridge::handlePaintPacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, PAINT_PAYLOAD_SIZE> &payload) {
	PaintCommand command;
	command.x = readU16LE(payload, 0);
	command.y = readU16LE(payload, 2);
	command.z = payload[4];
	command.itemId = readU16LE(payload, 5);

	BLOG("MV3D bridge RX 0x90 item {} @ {},{},{}", command.itemId, command.x, command.y, command.z);

	g_dispatcher().addEvent(
		[this, client = std::weak_ptr<asio::ip::tcp::socket>(client), command] {
			applyPaintCommand(client, command);
		},
		"MV3DEditorBridge::applyPaintCommand"
	);
}

void MV3DEditorBridge::handleSyncRequestPacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, SYNC_REQUEST_PAYLOAD_SIZE> &payload) {
	SyncRequest request;
	request.x = readU16LE(payload, 0);
	request.y = readU16LE(payload, 2);
	request.z = payload[4];
	request.range = payload[5];

	BLOG("MV3D bridge RX 0x93 center {},{},{} range {}", request.x, request.y, request.z, request.range);

	g_dispatcher().addEvent(
		[this, client = std::weak_ptr<asio::ip::tcp::socket>(client), request] {
			applySyncRequest(client, request);
		},
		"MV3DEditorBridge::applySyncRequest"
	);
}

void MV3DEditorBridge::handleErasePacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, ERASE_PAYLOAD_SIZE> &payload) {
	EraseCommand command;
	command.x = readU16LE(payload, 0);
	command.y = readU16LE(payload, 2);
	command.z = payload[4];
	command.itemId = readU16LE(payload, 5);

	BLOG("MV3D bridge RX 0x91 erase item {} @ {},{},{}", command.itemId, command.x, command.y, command.z);

	g_dispatcher().addEvent(
		[this, client = std::weak_ptr<asio::ip::tcp::socket>(client), command] {
			applyEraseCommand(client, command);
		},
		"MV3DEditorBridge::applyEraseCommand"
	);
}

void MV3DEditorBridge::handleEraseByStackPacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, ERASE_BY_STACK_PAYLOAD_SIZE> &payload) {
	EraseByStackCommand command;
	command.x = readU16LE(payload, 0);
	command.y = readU16LE(payload, 2);
	command.z = payload[4];
	command.itemId = readU16LE(payload, 5);
	command.stackIndex = payload[7];

	BLOG("MV3D bridge RX 0x95 erase-stack item {} stack {} @ {},{},{}",
		command.itemId,
		static_cast<int>(command.stackIndex),
		command.x,
		command.y,
		command.z);

	g_dispatcher().addEvent(
		[this, client = std::weak_ptr<asio::ip::tcp::socket>(client), command] {
			applyEraseByStackCommand(client, command);
		},
		"MV3DEditorBridge::applyEraseByStackCommand"
	);
}

void MV3DEditorBridge::handleMetadataUpdatePacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, METADATA_PAYLOAD_SIZE> &payload) {
	MetadataUpdateCommand command;
	command.x = readU16LE(payload, 0);
	command.y = readU16LE(payload, 2);
	command.z = payload[4];
	command.itemId = readU16LE(payload, 5);
	command.stackIndex = payload[7];
	command.actionId = readU16LE(payload, 8);
	command.uniqueId = readU16LE(payload, 10);
	command.transactionId = readU32LE(payload, 12);

	BLOG("MV3D bridge RX 0x98 item {} stack {} @ {},{},{} action {} unique {} tx {}",
		command.itemId,
		static_cast<int>(command.stackIndex),
		command.x,
		command.y,
		command.z,
		command.actionId,
		command.uniqueId,
		command.transactionId);

	g_dispatcher().addEvent(
		[this, client = std::weak_ptr<asio::ip::tcp::socket>(client), command] {
			applyMetadataUpdate(client, command);
		},
		"MV3DEditorBridge::applyMetadataUpdate"
	);
}

void MV3DEditorBridge::handleTextUpdatePacket(const std::shared_ptr<asio::ip::tcp::socket> &client, TextUpdateCommand command) {
	BLOG("MV3D bridge RX 0x99 item {} stack {} @ {},{},{} len {} tx {}",
		command.itemId,
		static_cast<int>(command.stackIndex),
		command.x,
		command.y,
		command.z,
		command.text.size(),
		command.transactionId);

	g_dispatcher().addEvent(
		[this, client = std::weak_ptr<asio::ip::tcp::socket>(client), command = std::move(command)]() mutable {
			applyTextUpdate(client, std::move(command));
		},
		"MV3DEditorBridge::applyTextUpdate"
	);
}

void MV3DEditorBridge::handleSaveRequestPacket(const std::shared_ptr<asio::ip::tcp::socket> &client) {
	BLOG("MV3D bridge RX 0x96 save request");

	bool expected = false;
	if (!g_isMapSaving.compare_exchange_strong(expected, true)) {
		sendSaveStatus(client, 2);
		BLOG("MV3D bridge TX 0x97 save status busy (2)");
		return;
	}

	g_dispatcher().addEvent(
		[this, client = std::weak_ptr<asio::ip::tcp::socket>(client)] {
			applySaveRequest(client);
		},
		"MV3DEditorBridge::applySaveRequest"
	);
}

void MV3DEditorBridge::handleLockRequestPacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, LOCK_REQUEST_PAYLOAD_SIZE> &payload) {
	LockRequest command;
	command.x = readU16LE(payload, 0);
	command.y = readU16LE(payload, 2);
	command.z = payload[4];
	command.lockMode = payload[5];
	command.leaseMs = readU16LE(payload, 6);

	BLOG("MV3D bridge RX 0x9B lock mode {} @ {},{},{} lease {}", 
		static_cast<int>(command.lockMode), command.x, command.y, static_cast<int>(command.z), command.leaseMs);

	g_dispatcher().addEvent(
		[this, client = std::weak_ptr<asio::ip::tcp::socket>(client), command] {
			applyLockRequest(client, command);
		},
		"MV3DEditorBridge::applyLockRequest"
	);
}

void MV3DEditorBridge::handlePresenceUpdatePacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, PRESENCE_UPDATE_PAYLOAD_SIZE> &payload) {
	PresenceUpdate command;
	command.x = readU16LE(payload, 0);
	command.y = readU16LE(payload, 2);
	command.z = payload[4];

	g_dispatcher().addEvent(
		[this, client = std::weak_ptr<asio::ip::tcp::socket>(client), command] {
			applyPresenceUpdate(client, command);
		},
		"MV3DEditorBridge::applyPresenceUpdate"
	);
}

void MV3DEditorBridge::handleEntityTriggerPacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, ENTITY_TRIGGER_PAYLOAD_SIZE> &payload) {
	EntityTrigger command;
	command.uniqueId = readU16LE(payload, 0);
	command.triggerType = payload[2];
	command.paramMs = readU16LE(payload, 3);

	g_dispatcher().addEvent(
		[this, client = std::weak_ptr<asio::ip::tcp::socket>(client), command] {
			applyEntityTrigger(client, command);
		},
		"MV3DEditorBridge::applyEntityTrigger"
	);
}

void MV3DEditorBridge::applyPaintCommand(const std::weak_ptr<asio::ip::tcp::socket> &client, const PaintCommand command) {
	const Position position(command.x, command.y, command.z);
	
	// Validar LOCK autoritativo
	{
		const uint64_t key = (static_cast<uint64_t>(command.x) << 32) | (static_cast<uint64_t>(command.y) << 16) | command.z;
		auto it = lockTable.find(key);
		if (it != lockTable.end() && it->second.expiresAt > std::chrono::steady_clock::now()) {
			uint16_t myId = 0;
			if (auto s = client.lock()) {
				std::scoped_lock lock(socketMutex);
				if (socketToUserId.contains(s)) {
					myId = socketToUserId[s];
				}
			}
			
			if (it->second.ownerId != myId) {
				logger.warn("MV3D editor bridge: paint REJECTED @ {} - locked by user {}", position.toString(), it->second.ownerId);
				if (auto s = client.lock()) {
					sendEditAck(s, 0, C2S_MAP_EDIT_PAINT, 1); // 1 = Locked/Error
				}
				return;
			}
		}
	}

	const auto tile = g_game().map.getTile(position);
	if (!tile) {
		logger.warn("MV3D editor bridge tile not found at {}", position.toString());
		return;
	}

	auto item = Item::CreateItem(command.itemId, 1);
	if (!item) {
		logger.warn("MV3D editor bridge could not create item {}", command.itemId);
		return;
	}

	const ReturnValue result = g_game().internalAddItem(tile, item, INDEX_WHEREEVER, FLAG_NOLIMIT);
	if (result != RETURNVALUE_NOERROR) {
		logger.warn("MV3D editor bridge failed to add item {} at {} with return value {}", command.itemId, position.toString(), static_cast<int>(result));
		return;
	}

	BLOG("MV3D bridge injected item {} at {}", command.itemId, position.toString());
	editLog.push_back({C2S_MAP_EDIT_PAINT, command.x, command.y, command.z, command.itemId});

	broadcastRawSyncUpdate(command);
}

void MV3DEditorBridge::applySyncRequest(const std::weak_ptr<asio::ip::tcp::socket> &client, const SyncRequest request) {
	if (!mapEditMode) {
		logger.warn("MV3D bridge rejected sync request {},{},{}: mv3dBridgeMapEditMode=false",
			request.x, request.y, static_cast<int>(request.z));
		return;
	}

	std::vector<SnapshotEntry> entries;

	const int32_t safeRange = std::clamp<int32_t>(request.range, 1, 15);
	const int32_t minX = std::max<int32_t>(0, static_cast<int32_t>(request.x) - safeRange);
	const int32_t maxX = std::min<int32_t>(std::numeric_limits<uint16_t>::max(), static_cast<int32_t>(request.x) + safeRange);
	const int32_t minY = std::max<int32_t>(0, static_cast<int32_t>(request.y) - safeRange);
	const int32_t maxY = std::min<int32_t>(std::numeric_limits<uint16_t>::max(), static_cast<int32_t>(request.y) + safeRange);

	for (int32_t x = minX; x <= maxX; ++x) {
		for (int32_t y = minY; y <= maxY; ++y) {
			const Position pos(static_cast<uint16_t>(x), static_cast<uint16_t>(y), request.z);
			const auto tile = g_game().map.getTile(pos);
			if (!tile) {
				continue;
			}

			if (const auto ground = tile->getGround()) {
				const uint16_t groundId = ground->getID();
				if (groundId != 0) {
					entries.push_back({ static_cast<uint16_t>(x), static_cast<uint16_t>(y), request.z, groundId });
				}
			}

			if (const auto* items = tile->getItemList()) {
				for (const auto &item : *items) {
					if (!item) {
						continue;
					}

					const uint16_t itemId = item->getID();
					if (itemId == 0) {
						continue;
					}

					entries.push_back({ static_cast<uint16_t>(x), static_cast<uint16_t>(y), request.z, itemId });
				}
			}
		}
	}

	if (entries.size() > std::numeric_limits<uint16_t>::max()) {
		logger.warn("MV3D editor bridge snapshot truncated from {} to {} entries", entries.size(), std::numeric_limits<uint16_t>::max());
		entries.resize(std::numeric_limits<uint16_t>::max());
	}

	BLOG("MV3D bridge snapshot prepared with {} entries", entries.size());

	if (const auto lockedClient = client.lock()) {
		sendSnapshot(lockedClient, entries);
	}

	// UTP chunk — mismos tiles mirrored a todos los suscriptores UDP.
	// playerX/Y/Z: usamos el centro del sync como aproximación (mejora: lookup por socket).
	broadcastChunk(entries, request, request.x, request.y, request.z);
}

void MV3DEditorBridge::applyEraseCommand(const std::weak_ptr<asio::ip::tcp::socket> &client, const EraseCommand command) {
	const Position position(command.x, command.y, command.z);

	// Validar LOCK autoritativo
	{
		const uint64_t key = (static_cast<uint64_t>(command.x) << 32) | (static_cast<uint64_t>(command.y) << 16) | command.z;
		auto it = lockTable.find(key);
		if (it != lockTable.end() && it->second.expiresAt > std::chrono::steady_clock::now()) {
			uint16_t myId = 0;
			if (auto s = client.lock()) {
				std::scoped_lock lock(socketMutex);
				if (socketToUserId.contains(s)) {
					myId = socketToUserId[s];
				}
			}
			if (it->second.ownerId != myId) {
				logger.warn("MV3D editor bridge: erase REJECTED @ {},{},{} - locked by user {}", command.x, command.y, static_cast<int>(command.z), it->second.ownerId);
				return;
			}
		}
	}

	const auto tile = g_game().map.getTile(position);
	if (!tile) {
		logger.warn("MV3D editor bridge erase tile not found at {}", position.toString());
		return;
	}

	std::shared_ptr<Item> targetItem;
	if (const auto* items = tile->getItemList(); items && !items->empty()) {
		for (auto it = items->rbegin(); it != items->rend(); ++it) {
			if (!(*it)) {
				continue;
			}

			if (command.itemId == 0 || (*it)->getID() == command.itemId) {
				targetItem = *it;
				break;
			}
		}
	}

	if (!targetItem) {
		BLOG("MV3D bridge erase ignored at {} (no removable match for item {})", position.toString(), command.itemId);
		return;
	}

	const uint16_t removedItemId = targetItem->getID();
	const ReturnValue result = g_game().internalRemoveItem(targetItem, 1);
	if (result != RETURNVALUE_NOERROR) {
		logger.warn("MV3D editor bridge failed to remove item {} at {} with return value {}", removedItemId, position.toString(), static_cast<int>(result));
		return;
	}

	BLOG("MV3D bridge removed item {} at {}", removedItemId, position.toString());
	editLog.push_back({C2S_MAP_EDIT_ERASE, command.x, command.y, command.z, removedItemId});

	PaintCommand update;
	update.x = command.x;
	update.y = command.y;
	update.z = command.z;
	update.itemId = 0; // 0 significa borrar o refrescar tile
	broadcastRawSyncUpdate(update);
}

void MV3DEditorBridge::applyEraseByStackCommand(const std::weak_ptr<asio::ip::tcp::socket> &client, const EraseByStackCommand command) {
	const Position position(command.x, command.y, command.z);
	
	// Validar LOCK autoritativo
	{
		const uint64_t key = (static_cast<uint64_t>(command.x) << 32) | (static_cast<uint64_t>(command.y) << 16) | command.z;
		auto it = lockTable.find(key);
		if (it != lockTable.end() && it->second.expiresAt > std::chrono::steady_clock::now()) {
			uint16_t myId = 0;
			if (auto s = client.lock()) {
				std::scoped_lock lock(socketMutex);
				if (socketToUserId.contains(s)) {
					myId = socketToUserId[s];
				}
			}
			if (it->second.ownerId != myId) {
				logger.warn("MV3D editor bridge: erase-stack REJECTED @ {},{},{} - locked by user {}", command.x, command.y, static_cast<int>(command.z), it->second.ownerId);
				return;
			}
		}
	}

	const auto tile = g_game().map.getTile(position);
	if (!tile) {
		logger.warn("MV3D editor bridge erase-stack tile not found at {}", position.toString());
		return;
	}

	const auto targetItem = tile->getItemByTopOrder(static_cast<int32_t>(command.stackIndex));
	if (!targetItem) {
		logger.warn("MV3D editor bridge erase-stack failed: stack {} out of bounds at {}",
			static_cast<int>(command.stackIndex),
			position.toString());
		return;
	}

	if (command.itemId != 0 && targetItem->getID() != command.itemId) {
		logger.warn("MV3D editor bridge erase-stack mismatch at {}: expected item {}, got {} (stack {})",
			position.toString(),
			command.itemId,
			targetItem->getID(),
			static_cast<int>(command.stackIndex));
		return;
	}

	const uint16_t removedItemId = targetItem->getID();
	const uint32_t removeCount = std::max<uint32_t>(1, targetItem->getItemCount());
	const ReturnValue result = g_game().internalRemoveItem(targetItem, removeCount);
	if (result != RETURNVALUE_NOERROR) {
		logger.warn("MV3D editor bridge erase-stack failed to remove item {} at {} with return value {}",
			removedItemId,
			position.toString(),
			static_cast<int>(result));
		return;
	}

	BLOG("MV3D bridge removed item {} at {} using stack {}",
		removedItemId,
		position.toString(),
		static_cast<int>(command.stackIndex));
	editLog.push_back({C2S_MAP_EDIT_ERASE, command.x, command.y, command.z, removedItemId});

	if (const auto lockedClient = client.lock()) {
		PaintCommand confirmation;
		confirmation.x = command.x;
		confirmation.y = command.y;
		confirmation.z = command.z;
		confirmation.itemId = 0;
		sendSyncConfirmation(lockedClient, confirmation);
	}
}

void MV3DEditorBridge::applyMetadataUpdate(const std::weak_ptr<asio::ip::tcp::socket> &client, const MetadataUpdateCommand command) {
	const Position position(command.x, command.y, command.z);
	const auto tile = g_game().map.getTile(position);
	if (!tile) {
		logger.warn("MV3D editor bridge metadata tile not found at {}", position.toString());
		if (const auto lockedClient = client.lock()) {
			sendEditAck(lockedClient, command.transactionId, C2S_ITEM_UPDATE_METADATA, 1);
		}
		return;
	}

	int32_t resolvedStackIndex = -1;
	const auto targetItem = resolveTileItemForEdit(tile, command.stackIndex, command.itemId, resolvedStackIndex);

	if (!targetItem) {
		logger.warn("MV3D editor bridge metadata target not found for item {} stack {} at {}",
			command.itemId,
			static_cast<int>(command.stackIndex),
			position.toString());
		if (const auto lockedClient = client.lock()) {
			sendEditAck(lockedClient, command.transactionId, C2S_ITEM_UPDATE_METADATA, 1);
		}
		return;
	}

	targetItem->setAttribute(ItemAttribute_t::ACTIONID, command.actionId);
	targetItem->setAttribute(ItemAttribute_t::UNIQUEID, command.uniqueId);
	BLOG("MV3D bridge updated metadata for item {} at {} (req stack {}, resolved stack {}, action {}, unique {})",
		command.itemId,
		position.toString(),
		static_cast<int>(command.stackIndex),
		resolvedStackIndex,
		command.actionId,
		command.uniqueId);

	if (const auto lockedClient = client.lock()) {
		sendEditAck(lockedClient, command.transactionId, C2S_ITEM_UPDATE_METADATA, 0);
	}
}

void MV3DEditorBridge::applyTextUpdate(const std::weak_ptr<asio::ip::tcp::socket> &client, TextUpdateCommand command) {
	const Position position(command.x, command.y, command.z);
	const auto tile = g_game().map.getTile(position);
	if (!tile) {
		logger.warn("MV3D editor bridge text tile not found at {}", position.toString());
		if (const auto lockedClient = client.lock()) {
			sendEditAck(lockedClient, command.transactionId, C2S_ITEM_UPDATE_TEXT, 1);
		}
		return;
	}

	int32_t resolvedStackIndex = -1;
	const auto targetItem = resolveTileItemForEdit(tile, command.stackIndex, command.itemId, resolvedStackIndex);

	if (!targetItem) {
		logger.warn("MV3D editor bridge text target not found for item {} stack {} at {}",
			command.itemId,
			static_cast<int>(command.stackIndex),
			position.toString());
		if (const auto lockedClient = client.lock()) {
			sendEditAck(lockedClient, command.transactionId, C2S_ITEM_UPDATE_TEXT, 1);
		}
		return;
	}

	const auto &itemType = Item::items[targetItem->getID()];
	if (!itemType.canWriteText) {
		logger.warn("MV3D editor bridge text update rejected for non-writable item {} at {}", targetItem->getID(), position.toString());
		if (const auto lockedClient = client.lock()) {
			sendEditAck(lockedClient, command.transactionId, C2S_ITEM_UPDATE_TEXT, 1);
		}
		return;
	}

	if (itemType.maxTextLen > 0 && command.text.size() > itemType.maxTextLen) {
		logger.warn("MV3D editor bridge text update rejected for item {} at {} (len {} > max {})",
			targetItem->getID(),
			position.toString(),
			command.text.size(),
			itemType.maxTextLen);
		if (const auto lockedClient = client.lock()) {
			sendEditAck(lockedClient, command.transactionId, C2S_ITEM_UPDATE_TEXT, 1);
		}
		return;
	}

	targetItem->setAttribute(ItemAttribute_t::TEXT, command.text);
	targetItem->setAttribute(ItemAttribute_t::WRITER, std::string("MV3D Editor"));
	targetItem->setAttribute(ItemAttribute_t::DATE,
		static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count()));

	BLOG("MV3D bridge updated text for item {} at {} (req stack {}, resolved stack {}, len {})",
		command.itemId,
		position.toString(),
		static_cast<int>(command.stackIndex),
		resolvedStackIndex,
		command.text.size());

	if (const auto lockedClient = client.lock()) {
		sendEditAck(lockedClient, command.transactionId, C2S_ITEM_UPDATE_TEXT, 0);
	}
}

void MV3DEditorBridge::applySaveRequest(const std::weak_ptr<asio::ip::tcp::socket> &client) {
	if (!mapEditMode) {
		logger.warn("MV3D bridge rejected map save: mv3dBridgeMapEditMode=false");
		if (const auto lockedClient = client.lock()) {
			sendSaveStatus(lockedClient, 1);
		}
		return;
	}

	uint8_t statusCode = 1;
	const auto mapPath = std::filesystem::path(g_configManager().getString(DATA_DIRECTORY))
		/ "world"
		/ (g_configManager().getString(MAP_NAME) + ".otbm");

	std::filesystem::file_time_type beforeWriteTime {};
	std::filesystem::file_time_type afterWriteTime {};
	bool hasBeforeTimestamp = false;

	try {
		std::error_code ec;
		if (std::filesystem::exists(mapPath, ec) && !ec) {
			beforeWriteTime = std::filesystem::last_write_time(mapPath, ec);
			hasBeforeTimestamp = !ec;
			if (ec) {
				logger.warn("MV3D editor bridge could not read pre-save timestamp for {}: {}", mapPath.string(), ec.message());
			}
		} else {
			logger.warn("MV3D editor bridge map file missing before save check: {}", mapPath.string());
		}

		const bool logicalSaveOk = Map::save();
		bool physicalSaveOk = hasBeforeTimestamp && didMapBinaryTimestampAdvance(mapPath, beforeWriteTime, afterWriteTime);

		if (logicalSaveOk && !physicalSaveOk && hasBeforeTimestamp && tryDeepBinarySaveViaCommand(mapPath)) {
			physicalSaveOk = didMapBinaryTimestampAdvance(mapPath, beforeWriteTime, afterWriteTime);
		}

		statusCode = (logicalSaveOk && physicalSaveOk) ? 0 : 1;

		if (statusCode == 0) {
			// Save exitoso: limpiar journal de crash-recovery y liberar memoria
			const std::string dir = g_configManager().getString(DATA_DIRECTORY) + "/world";
			const std::string journalPath = dir + "/mv3d_edits.json";
			std::error_code removeEc;
			std::filesystem::remove(journalPath, removeEc);
			editLog.clear();
			BLOG("MV3D bridge crash-recovery journal limpiado tras save exitoso");
		} else {
			// Save falló: persistir edits pendientes para recuperacion tras crash
			flushEditsToDisk();
		}

		if (logicalSaveOk && !physicalSaveOk) {
			logger.warn("MV3D editor bridge save returned logical success but .otbm timestamp did not advance (before={}, after={})",
				std::chrono::duration_cast<std::chrono::milliseconds>(beforeWriteTime.time_since_epoch()).count(),
				std::chrono::duration_cast<std::chrono::milliseconds>(afterWriteTime.time_since_epoch()).count());
		}
	} catch (const std::exception &ex) {
		logger.error("MV3D editor bridge Map::save exception: {}", ex.what());
		statusCode = 1;
	} catch (...) {
		logger.error("MV3D editor bridge Map::save unknown exception");
		statusCode = 1;
	}

	g_isMapSaving.store(false);

	if (const auto lockedClient = client.lock()) {
		sendSaveStatus(lockedClient, statusCode);
	}

	BLOG("MV3D bridge TX 0x97 save status {}", statusCode == 0 ? "ok (0)" : "error (1)");
}

bool MV3DEditorBridge::didMapBinaryTimestampAdvance(const std::filesystem::path &mapPath, const std::filesystem::file_time_type &beforeWriteTime, std::filesystem::file_time_type &afterWriteTime) const {
	std::error_code ec;
	afterWriteTime = std::filesystem::last_write_time(mapPath, ec);
	if (ec) {
		logger.warn("MV3D editor bridge could not read post-save timestamp for {}: {}", mapPath.string(), ec.message());
		return false;
	}

	return afterWriteTime > beforeWriteTime;
}

bool MV3DEditorBridge::tryDeepBinarySaveViaCommand(const std::filesystem::path &mapPath) const {
	const char* cmdTemplate = std::getenv("MV3D_EDITOR_DEEP_SAVE_CMD");
	if (!cmdTemplate || std::string_view(cmdTemplate).empty()) {
		return false;
	}

	std::string command = cmdTemplate;
	const std::string placeholder = "{map}";
	if (const auto pos = command.find(placeholder); pos != std::string::npos) {
		command.replace(pos, placeholder.size(), '"' + mapPath.string() + '"');
	}

	BLOG("MV3D bridge trying deep binary save command: {}", command);
	const auto startedAt = std::chrono::steady_clock::now();
	const int exitCode = std::system(command.c_str());
	const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - startedAt).count();
	if (exitCode != 0) {
		logger.warn("MV3D editor bridge deep binary save command failed with exit code {} after {}ms", exitCode, elapsedMs);
		return false;
	}

	BLOG("MV3D bridge deep binary save completed successfully in {}ms", elapsedMs);
	return true;
}

bool MV3DEditorBridge::flushEditsToDisk() {
	const std::string dir = g_configManager().getString(DATA_DIRECTORY) + "/world";
	const std::string path = dir + "/mv3d_edits.json";
	const std::string tmpPath = path + ".tmp";

	try {
		std::ostringstream ss;
		ss << "{\n  \"version\": 1,\n  \"edits\": [";
		for (size_t i = 0; i < editLog.size(); ++i) {
			const auto &e = editLog[i];
			if (i > 0) {
				ss << ',';
			}
			ss << "\n    {\"op\": \"" << (e.op == C2S_MAP_EDIT_PAINT ? "paint" : "erase")
			   << "\", \"x\": " << e.x
			   << ", \"y\": " << e.y
			   << ", \"z\": " << static_cast<int>(e.z)
			   << ", \"id\": " << e.itemId << '}';
		}
		ss << "\n  ]\n}\n";

		{
			std::ofstream out(tmpPath, std::ios::trunc);
			if (!out.is_open()) {
				logger.error("MV3D editor bridge cannot open {} for writing", tmpPath);
				return false;
			}
			out << ss.str();
		}

		std::filesystem::rename(tmpPath, path);
		BLOG("MV3D bridge flushed {} edits to {}", editLog.size(), path);
		return true;
	} catch (const std::exception &ex) {
		logger.error("MV3D editor bridge flushEditsToDisk failed: {}", ex.what());
		return false;
	}
}

void MV3DEditorBridge::loadEditsFromDisk() {
	const std::string path = g_configManager().getString(DATA_DIRECTORY) + "/world/mv3d_edits.json";

	if (!std::filesystem::exists(path)) {
		BLOG("MV3D bridge: no persisted edits at {}, starting fresh", path);
		return;
	}

	std::ifstream file(path);
	if (!file.is_open()) {
		logger.error("MV3D editor bridge: cannot read edits file {}", path);
		return;
	}

	const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

	const size_t arrayStart = content.find('[');
	if (arrayStart == std::string::npos) {
		logger.warn("MV3D editor bridge: edits file has no array, skipping replay");
		return;
	}
	const size_t arrayEnd = content.rfind(']');
	if (arrayEnd == std::string::npos || arrayEnd <= arrayStart) {
		logger.warn("MV3D editor bridge: edits file array is malformed, skipping replay");
		return;
	}

	auto extractStr = [](const std::string &obj, const std::string &key) -> std::string {
		const std::string search = "\"" + key + "\": \"";
		const size_t p = obj.find(search);
		if (p == std::string::npos) {
			return {};
		}
		const size_t valueStart = p + search.size();
		const size_t valueEnd = obj.find('"', valueStart);
		if (valueEnd == std::string::npos) {
			return {};
		}
		return obj.substr(valueStart, valueEnd - valueStart);
	};

	auto extractInt = [](const std::string &obj, const std::string &key) -> int {
		const std::string search = "\"" + key + "\": ";
		const size_t p = obj.find(search);
		if (p == std::string::npos) {
			return -1;
		}
		return std::atoi(obj.c_str() + p + search.size());
	};

	int applied = 0;
	int skipped = 0;
	size_t pos = arrayStart + 1;

	while (pos < arrayEnd) {
		const size_t objStart = content.find('{', pos);
		if (objStart == std::string::npos || objStart >= arrayEnd) {
			break;
		}
		const size_t objEnd = content.find('}', objStart);
		if (objEnd == std::string::npos || objEnd > arrayEnd) {
			break;
		}

		const std::string obj = content.substr(objStart, objEnd - objStart + 1);
		const std::string opStr = extractStr(obj, "op");
		const int x = extractInt(obj, "x");
		const int y = extractInt(obj, "y");
		const int z = extractInt(obj, "z");
		const int id = extractInt(obj, "id");

		const bool valid = !opStr.empty()
			&& x >= 0 && x <= 65535
			&& y >= 0 && y <= 65535
			&& z >= 0 && z <= 15
			&& id > 0 && id <= 65535;

		if (valid) {
			const uint16_t ux = static_cast<uint16_t>(x);
			const uint16_t uy = static_cast<uint16_t>(y);
			const uint8_t  uz = static_cast<uint8_t>(z);
			const uint16_t uid = static_cast<uint16_t>(id);
			const Position position(ux, uy, uz);

			if (opStr == "paint") {
				const auto tile = g_game().map.getTile(position);
				if (tile) {
					auto item = Item::CreateItem(uid, 1);
					if (item && g_game().internalAddItem(tile, item, INDEX_WHEREEVER, FLAG_NOLIMIT) == RETURNVALUE_NOERROR) {
						editLog.push_back({C2S_MAP_EDIT_PAINT, ux, uy, uz, uid});
						++applied;
					} else {
						logger.warn("MV3D editor bridge replay: could not add item {} at {},{},{}", uid, ux, uy, static_cast<int>(uz));
						++skipped;
					}
				} else {
					logger.warn("MV3D editor bridge replay: tile not found at {},{},{} for paint", ux, uy, static_cast<int>(uz));
					++skipped;
				}
			} else {
				const auto tile = g_game().map.getTile(position);
				if (tile) {
					std::shared_ptr<Item> targetItem;
					if (const auto* items = tile->getItemList(); items && !items->empty()) {
						for (auto it = items->rbegin(); it != items->rend(); ++it) {
							if (*it && (*it)->getID() == uid) {
								targetItem = *it;
								break;
							}
						}
					}
					if (targetItem && g_game().internalRemoveItem(targetItem, 1) == RETURNVALUE_NOERROR) {
						editLog.push_back({C2S_MAP_EDIT_ERASE, ux, uy, uz, uid});
						++applied;
					} else {
						logger.warn("MV3D editor bridge replay: could not remove item {} at {},{},{}", uid, ux, uy, static_cast<int>(uz));
						++skipped;
					}
				} else {
					logger.warn("MV3D editor bridge replay: tile not found at {},{},{} for erase", ux, uy, static_cast<int>(uz));
					++skipped;
				}
			}
		} else {
			++skipped;
		}

		pos = objEnd + 1;
	}

	BLOG("MV3D bridge replay complete: {} applied, {} skipped (source: {})", applied, skipped, path);
}

void MV3DEditorBridge::sendSyncConfirmation(const std::shared_ptr<asio::ip::tcp::socket> &client, const PaintCommand &command) {
	std::array<uint8_t, SYNC_CONFIRMATION_SIZE> response {};
	response[0] = S2C_MAP_SYNC_UPDATE;
	writeU16LE(response, 1, command.x);
	writeU16LE(response, 3, command.y);
	response[5] = command.z;
	writeU16LE(response, 6, command.itemId);

	std::scoped_lock lock(socketMutex);
	if (!client->is_open()) {
		return;
	}
	asio::error_code ec;
	asio::write(*client, asio::buffer(response), ec);
	if (ec) {
		logger.warn("MV3D editor bridge failed to send 0x92: {}", ec.message());
		return;
	}

	BLOG("MV3D bridge TX 0x92 item {} @ {},{},{}", command.itemId, command.x, command.y, command.z);
}

void MV3DEditorBridge::sendSnapshot(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::vector<SnapshotEntry> &entries) {
	const auto count = static_cast<uint16_t>(entries.size());
	std::vector<uint8_t> payload(3 + static_cast<size_t>(count) * 7);
	payload[0] = S2C_MAP_SYNC_SNAPSHOT;
	writeU16LE(payload, 1, count);

	size_t offset = 3;
	for (const auto &entry : entries) {
		writeU16LE(payload, offset, entry.x);
		writeU16LE(payload, offset + 2, entry.y);
		payload[offset + 4] = entry.z;
		writeU16LE(payload, offset + 5, entry.itemId);
		offset += 7;
	}

	std::scoped_lock lock(socketMutex);
	if (!client->is_open()) {
		return;
	}
	asio::error_code ec;
	asio::write(*client, asio::buffer(payload), ec);
	if (ec) {
		logger.warn("MV3D editor bridge failed to send 0x94: {}", ec.message());
		return;
	}

	BLOG("MV3D bridge TX 0x94 snapshot entries {}", count);
}

void MV3DEditorBridge::sendEditAck(const std::shared_ptr<asio::ip::tcp::socket> &client, const uint32_t transactionId, const uint8_t sourceOpcode, const uint8_t statusCode) {
	std::array<uint8_t, EDIT_ACK_SIZE> response {};
	response[0] = S2C_EDIT_ACK;
	response[1] = static_cast<uint8_t>(transactionId & 0xFF);
	response[2] = static_cast<uint8_t>((transactionId >> 8) & 0xFF);
	response[3] = static_cast<uint8_t>((transactionId >> 16) & 0xFF);
	response[4] = static_cast<uint8_t>((transactionId >> 24) & 0xFF);
	response[5] = sourceOpcode;
	response[6] = statusCode;

	std::scoped_lock lock(socketMutex);
	if (!client->is_open()) {
		return;
	}
	asio::error_code ec;
	asio::write(*client, asio::buffer(response), ec);
	if (ec) {
		logger.warn("MV3D editor bridge failed to send 0x9A: {}", ec.message());
		return;
	}

	BLOG("MV3D bridge TX 0x9A tx {} source 0x{:02X} status {}", transactionId, static_cast<int>(sourceOpcode), static_cast<int>(statusCode));
}

void MV3DEditorBridge::sendSaveStatus(const std::shared_ptr<asio::ip::tcp::socket> &client, const uint8_t statusCode) {
	std::array<uint8_t, SAVE_STATUS_SIZE> response {};
	response[0] = S2C_MAP_SAVE_STATUS;
	response[1] = statusCode;

	std::scoped_lock lock(socketMutex);
	if (!client->is_open()) {
		return;
	}
	asio::error_code ec;
	asio::write(*client, asio::buffer(response), ec);
	if (ec) {
		logger.warn("MV3D editor bridge failed to send 0x97: {}", ec.message());
		return;
	}
}

// =============================================================================
// HELPERS INTERNOS
// =============================================================================

namespace {
void writeU32LE_v(std::vector<uint8_t> &buf, size_t offset, uint32_t v) {
	buf[offset]     = static_cast<uint8_t>(v & 0xFF);
	buf[offset + 1] = static_cast<uint8_t>((v >> 8)  & 0xFF);
	buf[offset + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
	buf[offset + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
void writeI32LE_v(std::vector<uint8_t> &buf, size_t offset, int32_t v) {
	writeU32LE_v(buf, offset, static_cast<uint32_t>(v));
}
void writeI16LE_v(std::vector<uint8_t> &buf, size_t offset, int16_t v) {
	buf[offset]     = static_cast<uint8_t>(static_cast<uint16_t>(v) & 0xFF);
	buf[offset + 1] = static_cast<uint8_t>((static_cast<uint16_t>(v) >> 8) & 0xFF);
}
void writeU16LE_v(std::vector<uint8_t> &buf, size_t offset, uint16_t v) {
	buf[offset]     = static_cast<uint8_t>(v & 0xFF);
	buf[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void writeF32LE_v(std::vector<uint8_t> &buf, size_t offset, float v) {
	uint32_t uv;
	std::memcpy(&uv, &v, 4);
	writeU32LE_v(buf, offset, uv);
}
} // namespace

void MV3DEditorBridge::broadcastRaw(const std::vector<uint8_t> &data) {
	std::scoped_lock lock(socketMutex);
	for (auto it = clients.begin(); it != clients.end(); ) {
		auto &client = *it;
		if (client && client->is_open()) {
			asio::error_code ec;
			asio::write(*client, asio::buffer(data), ec);
			if (ec) {
				logger.warn("MV3D editor bridge broadcastRaw failed for a client: {}", ec.message());
				it = clients.erase(it);
				continue;
			}
			++it;
		} else {
			it = clients.erase(it);
		}
	}
}

void MV3DEditorBridge::broadcastUTP(const std::string &json, bool highPriority) {
	if (!running || json.empty()) {
		return;
	}

	// Optimización: si no hay suscriptores, no fragmentar ni encolar
	{
		std::scoped_lock lock(utpMutex);
		if (utpSubscribers.empty()) {
			return;
		}
	}

	try {
		if (json.size() > UTP_SAFE_MTU && json.find("\"kind\":\"fragment\"") == std::string::npos) {
			const auto fragmentId = ++utpFragmentSeq;
			const auto count = static_cast<uint16_t>((json.size() + UTP_FRAGMENT_PAYLOAD_SIZE - 1) / UTP_FRAGMENT_PAYLOAD_SIZE);
			
			if (count > 10000) {
				logger.warn("[UTP] Payload masivo ({} bytes), abortando fragmentación para proteger el servidor.", json.size());
				return;
			}

			for (uint16_t index = 0; index < count; ++index) {
				const auto offset = static_cast<size_t>(index) * UTP_FRAGMENT_PAYLOAD_SIZE;
				enqueueUTPPacket(mv3d::utp::BuildFragment(
					fragmentId,
					index,
					count,
					json.substr(offset, UTP_FRAGMENT_PAYLOAD_SIZE)
				), highPriority);
			}
			return;
		}

		enqueueUTPPacket(json, highPriority);
	} catch (const std::exception &e) {
		logger.error("[UTP] Excepción en broadcastUTP: {}", e.what());
	} catch (...) {
		logger.error("[UTP] Error fatal desconocido en broadcastUTP");
	}
}

void MV3DEditorBridge::enqueueUTPPacket(const std::string &json, bool highPriority) {
	{
		std::scoped_lock lock(utpQueueMutex);
		const auto queued = utpHighPriorityQueue.size() + utpNormalPriorityQueue.size();
		if (queued >= utpMaxQueue) {
			if (!utpNormalPriorityQueue.empty()) {
				utpNormalPriorityQueue.pop_front();
			} else if (!utpHighPriorityQueue.empty()) {
				utpHighPriorityQueue.pop_front();
			}
		}

		if (highPriority) {
			utpHighPriorityQueue.push_back(json);
		} else {
			utpNormalPriorityQueue.push_back(json);
		}
	}
	utpQueueCv.notify_one();
}

void MV3DEditorBridge::registerUTPSubscriber(const asio::ip::udp::endpoint &ep) {
	std::scoped_lock lock(utpMutex);
	const auto key = endpointKey(ep);
	auto it = utpSubscribers.find(key);
	if (it != utpSubscribers.end()) {
		it->second.lastSeen = std::chrono::steady_clock::now();
	} else {
		utpSubscribers[key] = { ep, std::chrono::steady_clock::now() };
	}
}

void MV3DEditorBridge::pruneUTPSubscribers() {
	const auto now = std::chrono::steady_clock::now();
	std::scoped_lock lock(utpMutex);
	size_t removed = 0;
	for (auto it = utpSubscribers.begin(); it != utpSubscribers.end(); ) {
		if (now - it->second.lastSeen >= UTP_SUBSCRIBER_TTL) {
			it = utpSubscribers.erase(it);
			++removed;
		} else {
			++it;
		}
	}
	if (removed > 0) {
		BLOG("MV3D bridge UTP pruned {} stale subscriber(s)", removed);
	}
}

void MV3DEditorBridge::broadcastRawSyncUpdate(const PaintCommand &command) {
	std::array<uint8_t, SYNC_CONFIRMATION_SIZE> response {};
	response[0] = S2C_MAP_SYNC_UPDATE;
	writeU16LE(response, 1, command.x);
	writeU16LE(response, 3, command.y);
	response[5] = command.z;
	writeU16LE(response, 6, command.itemId);
	
	std::vector<uint8_t> buf(response.begin(), response.end());
	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildMapUpdate(command.x, command.y, command.z, command.itemId));
}

void MV3DEditorBridge::sendItemUseResult(const std::shared_ptr<asio::ip::tcp::socket> &client,
                                          uint32_t containerId, uint32_t itemId,
                                          uint8_t approved, uint8_t reasonCode) {
	if (!client) return;
	// opcode(1) + containerId(4) + itemId(4) + approved(1) + reasonCode(1) = 11
	std::vector<uint8_t> buf(11, 0);
	buf[0] = S2C_ITEM_USE_RESULT;
	writeU32LE_v(buf, 1, containerId);
	writeU32LE_v(buf, 5, itemId);
	buf[9]  = approved;
	buf[10] = reasonCode;

	std::scoped_lock lock(socketMutex);
	if (!client->is_open()) return;
	asio::error_code ec;
	asio::write(*client, asio::buffer(buf), ec);
	if (ec) {
		logger.warn("MV3D editor bridge failed to send 0xB4: {}", ec.message());
	}
}

// =============================================================================
// HANDLERS C→S — GAMEPLAY (0xB0-0xBC)
// =============================================================================

void MV3DEditorBridge::handleAttackPacket(const std::shared_ptr<asio::ip::tcp::socket> &client,
                                           const std::array<uint8_t, ATTACK_PAYLOAD_SIZE> &payload) {
	AttackCommand command;
	command.targetId = static_cast<uint32_t>(payload[0])
	                 | (static_cast<uint32_t>(payload[1]) << 8)
	                 | (static_cast<uint32_t>(payload[2]) << 16)
	                 | (static_cast<uint32_t>(payload[3]) << 24);
	BLOG("MV3D bridge RX 0xB0 attack target={}", command.targetId);
	g_dispatcher().addEvent(
		[this, client = std::weak_ptr<asio::ip::tcp::socket>(client), command] {
			applyAttackCommand(client, command);
		},
		"MV3DEditorBridge::applyAttackCommand"
	);
}

void MV3DEditorBridge::handleItemUsePacket(const std::shared_ptr<asio::ip::tcp::socket> &client,
                                            const std::array<uint8_t, ITEM_USE_PAYLOAD_SIZE> &payload) {
	ItemUseCommand command;
	command.containerId = static_cast<uint32_t>(payload[0]) | (static_cast<uint32_t>(payload[1]) << 8)
	                    | (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24);
	command.itemId      = static_cast<uint32_t>(payload[4]) | (static_cast<uint32_t>(payload[5]) << 8)
	                    | (static_cast<uint32_t>(payload[6]) << 16) | (static_cast<uint32_t>(payload[7]) << 24);
	BLOG("MV3D bridge RX 0xB3 item use container={} item={}", command.containerId, command.itemId);
	g_dispatcher().addEvent(
		[this, client = std::weak_ptr<asio::ip::tcp::socket>(client), command] {
			applyItemUseCommand(client, command);
		},
		"MV3DEditorBridge::applyItemUseCommand"
	);
}

void MV3DEditorBridge::handleDialogChoicePacket(const std::shared_ptr<asio::ip::tcp::socket> &client,
                                                 const std::array<uint8_t, DIALOG_CHOICE_PAYLOAD_SIZE> &payload) {
	DialogChoiceCommand command;
	command.nodeId      = static_cast<uint32_t>(payload[0]) | (static_cast<uint32_t>(payload[1]) << 8)
	                    | (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24);
	command.optionIndex = payload[4];
	BLOG("MV3D bridge RX 0xB6 dialog choice node={} option={}", command.nodeId, static_cast<int>(command.optionIndex));
	g_dispatcher().addEvent(
		[this, client = std::weak_ptr<asio::ip::tcp::socket>(client), command] {
			applyDialogChoiceCommand(client, command);
		},
		"MV3DEditorBridge::applyDialogChoiceCommand"
	);
}

void MV3DEditorBridge::handleAiBehaviorReportPacket(const std::shared_ptr<asio::ip::tcp::socket> &/*client*/,
                                                     const std::array<uint8_t, 10> &payload) {
	AiBehaviorReport report;
	report.entityId = static_cast<uint32_t>(payload[0]) | (static_cast<uint32_t>(payload[1]) << 8)
	                | (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24);
	report.tileX    = static_cast<int16_t>(payload[4] | (payload[5] << 8));
	report.tileY    = static_cast<int16_t>(payload[6] | (payload[7] << 8));
	report.floor    = payload[8];
	report.state    = payload[9];
	BLOG("MV3D bridge RX 0xB9 AI report entity={} pos={},{},{} state={}",
		report.entityId, report.tileX, report.tileY, static_cast<int>(report.floor), static_cast<int>(report.state));
	// TODO CODEX: si se requiere reconciliación autoritativa, despachar al main thread
}

void MV3DEditorBridge::handleBossPhaseReportPacket(const std::shared_ptr<asio::ip::tcp::socket> &/*client*/,
                                                    const std::array<uint8_t, BOSS_REPORT_PAYLOAD_SIZE> &payload) {
	BossPhaseReport report;
	report.entityId = static_cast<uint32_t>(payload[0]) | (static_cast<uint32_t>(payload[1]) << 8)
	                | (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24);
	report.phase    = payload[4];
	report.hp       = static_cast<int32_t>(static_cast<uint32_t>(payload[5]) | (static_cast<uint32_t>(payload[6]) << 8)
	               | (static_cast<uint32_t>(payload[7]) << 16) | (static_cast<uint32_t>(payload[8]) << 24));
	BLOG("MV3D bridge RX 0xBB boss report entity={} phase={} hp={}",
		report.entityId, static_cast<int>(report.phase), report.hp);
	// TODO CODEX: si se requiere reconciliación autoritativa, despachar al main thread
}

void MV3DEditorBridge::handleChatMessagePacket(const std::shared_ptr<asio::ip::tcp::socket> &client) {
	std::array<uint8_t, 2> lenBuf {};
	asio::error_code ec;
	asio::read(*client, asio::buffer(lenBuf), ec);
	if (ec) return;

	uint16_t len = readU16LE(lenBuf, 0);
	if (len > 1024) return;

	std::vector<char> msgBuf(len);
	asio::read(*client, asio::buffer(msgBuf), ec);
	if (ec) return;

	ChatMessageCommand cmd;
	cmd.message = std::string(msgBuf.begin(), msgBuf.end());

	g_dispatcher().addEvent([this, client, cmd]() {
		applyChatMessageCommand(client, cmd);
	}, __FUNCTION__);
}

void MV3DEditorBridge::applyChatMessageCommand(const std::weak_ptr<asio::ip::tcp::socket> &client, ChatMessageCommand command) {
	uint16_t myId = 0;
	if (auto s = client.lock()) {
		std::scoped_lock lock(socketMutex);
		if (socketToUserId.contains(s)) {
			myId = socketToUserId[s];
		}
	}
	if (myId == 0) return;

	BLOG("MV3D bridge CHAT from user {}: {}", myId, command.message);
	broadcastChatMessage(myId, command.message);
}

void MV3DEditorBridge::handleListenerUpdatePacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, 24> &payload) {
	ListenerUpdateCommand cmd;
	std::memcpy(&cmd.posX, &payload[0], 4);
	std::memcpy(&cmd.posY, &payload[4], 4);
	std::memcpy(&cmd.posZ, &payload[8], 4);
	std::memcpy(&cmd.dirX, &payload[12], 4);
	std::memcpy(&cmd.dirY, &payload[16], 4);
	std::memcpy(&cmd.dirZ, &payload[20], 4);

	g_dispatcher().addEvent([this, client, cmd]() {
		applyListenerUpdateCommand(client, cmd);
	}, __FUNCTION__);
}

void MV3DEditorBridge::applyListenerUpdateCommand(const std::weak_ptr<asio::ip::tcp::socket> &client, ListenerUpdateCommand command) {
	// Futuro: usar para audio-occlusion server-side o visibilidad proactiva
}

// =============================================================================
// APPLY — GAMEPLAY (main thread via g_dispatcher)
// =============================================================================

void MV3DEditorBridge::applyAttackCommand(const std::weak_ptr<asio::ip::tcp::socket> &/*client*/,
                                           const AttackCommand command) {
	if (!playerOperationMode) {
		logger.warn("MV3D editor bridge rejected 0xB0 attack target={} because mv3dBridgePlayerOperationMode=false", command.targetId);
		return;
	}

	if (mv3dPlayerId == 0) {
		logger.warn("MV3D editor bridge rejected 0xB0 attack target={} because MV3D_PLAYER_ID is not set", command.targetId);
		return;
	}

	const auto attacker = g_game().getPlayerByID(mv3dPlayerId);
	const auto target = g_game().getCreatureByID(command.targetId);
	if (!attacker || !target) {
		logger.warn("MV3D editor bridge rejected 0xB0 attack player={} target={} attacker_found={} target_found={}",
			mv3dPlayerId, command.targetId, static_cast<bool>(attacker), static_cast<bool>(target));
		return;
	}

	g_game().playerSetAttackedCreature(attacker->getID(), target->getID());
	BLOG("MV3D bridge apply 0xB0 attack player={} target={}", attacker->getID(), target->getID());
}

void MV3DEditorBridge::applyItemUseCommand(const std::weak_ptr<asio::ip::tcp::socket> &client,
                                            const ItemUseCommand command) {
	if (!playerOperationMode) {
		logger.warn("MV3D bridge item use REJECTED: mv3dBridgePlayerOperationMode=false");
		if (const auto lockedClient = client.lock()) {
			sendItemUseResult(lockedClient, command.containerId, command.itemId, 0 /*approved*/, 2);
		}
		return;
	}

	if (mv3dPlayerId == 0) return;

	const auto player = g_game().getPlayerByID(mv3dPlayerId);
	if (!player) {
		logger.warn("MV3D bridge item use REJECTED: player {} not found", mv3dPlayerId);
		return;
	}

	// Si containerId es 0xFFFF, es un item del mundo o un uniqueId
	// En MV3D usualmente enviamos el uniqueId en itemId si es un objeto estático
	if (command.containerId == 0xFFFFFFFF) {
		const auto item = g_game().getUniqueItem(command.itemId);
		if (item) {
			BLOG("MV3D bridge apply item use (World UID {}) for player {}", command.itemId, player->getName());
			g_game().playerUseItem(player->getID(), item->getPosition(), 255, 0, item->getID());
		}
	} else {
		// Uso de item en inventario/container
		BLOG("MV3D bridge apply item use (CID {} Item {}) for player {}", 
			command.containerId, command.itemId, player->getName());
		// Canary usa slot/index para inventario, este hook se expandirá según la UI de inventario D3D
	}

	if (const auto lockedClient = client.lock()) {
		sendItemUseResult(lockedClient, command.containerId, command.itemId, 1 /*approved*/, 0);
	}
}

void MV3DEditorBridge::applyDialogChoiceCommand(const std::weak_ptr<asio::ip::tcp::socket> &/*client*/,
                                                 const DialogChoiceCommand command) {
	if (!playerOperationMode) {
		logger.warn("MV3D bridge dialog choice REJECTED: mv3dBridgePlayerOperationMode=false");
		return;
	}

	if (mv3dPlayerId == 0) return;

	const auto player = g_game().getPlayerByID(mv3dPlayerId);
	if (!player) return;

	const auto npc = player->getShopOwner(); // O el NPC con el que está interactuando
	if (npc) {
		BLOG("MV3D bridge apply dialog choice (Node {} Opt {}) via player {} with NPC {}", 
			command.nodeId, static_cast<int>(command.optionIndex), player->getName(), npc->getName());
		npc->onDialogChoice(player, command.nodeId, command.optionIndex);
	} else {
		logger.warn("MV3D bridge dialog choice REJECTED: player {} not interacting with NPC", player->getName());
	}
}

void MV3DEditorBridge::applyLockRequest(const std::weak_ptr<asio::ip::tcp::socket> &client, LockRequest command) {
	const uint64_t key = (static_cast<uint64_t>(command.x) << 32) | (static_cast<uint64_t>(command.y) << 16) | command.z;
	const auto now = std::chrono::steady_clock::now();
	
	uint16_t myId = 0;
	if (auto s = client.lock()) {
		std::scoped_lock lock(socketMutex);
		if (socketToUserId.contains(s)) {
			myId = socketToUserId[s];
		}
	}
	if (myId == 0) return;

	auto it = lockTable.find(key);
	// Limpiar locks expirados
	if (it != lockTable.end() && it->second.expiresAt <= now) {
		lockTable.erase(it);
		it = lockTable.end();
	}

	if (command.lockMode == 0 || command.lockMode == 2) { // ACQUIRE / FORCE
		if (it != lockTable.end() && it->second.ownerId != myId && command.lockMode != 2) {
			BLOG("MV3D bridge: lock REJECTED @ {},{},{} - owned by user {}", command.x, command.y, static_cast<int>(command.z), it->second.ownerId);
			if (auto s = client.lock()) {
				uint16_t retryAfter = static_cast<uint16_t>(std::chrono::duration_cast<std::chrono::milliseconds>(it->second.expiresAt - now).count());
				sendLockDenied(s, command.x, command.y, command.z, retryAfter, static_cast<uint16_t>(it->second.ownerId));
			}
			return;
		}
		
		uint16_t lease = std::clamp<uint16_t>(command.leaseMs, 500, 30000);
		lockTable[key] = {myId, now + std::chrono::milliseconds(lease)};
		broadcastUTP(mv3d::utp::BuildLock(myId, command.x, command.y, command.z, command.lockMode, lease));
		BLOG("MV3D bridge: tile LOCKED @ {},{},{} by user {} (lease {}ms)", command.x, command.y, static_cast<int>(command.z), myId, lease);
	} else if (command.lockMode == 1) { // RELEASE
		if (it != lockTable.end() && it->second.ownerId == myId) {
			lockTable.erase(it);
			broadcastUTP(mv3d::utp::BuildLock(myId, command.x, command.y, command.z, command.lockMode, 0));
			BLOG("MV3D bridge: tile UNLOCKED @ {},{},{} by user {}", command.x, command.y, static_cast<int>(command.z), myId);
		}
	}
}

void MV3DEditorBridge::applyPresenceUpdate(const std::weak_ptr<asio::ip::tcp::socket> &client, PresenceUpdate command) {
	uint16_t myId = 0;
	if (auto s = client.lock()) {
		std::scoped_lock lock(socketMutex);
		if (socketToUserId.contains(s)) {
			myId = socketToUserId[s];
		} else {
			myId = nextUserId++;
			socketToUserId[s] = myId;
		}
	}
	
	if (myId == 0) return;

	presenceTable[myId] = {myId, command.x, command.y, command.z};

	// Sincronizar con el Player de Canary si está configurado
	if (mv3dPlayerId != 0) {
		if (auto player = g_game().getPlayerByID(mv3dPlayerId)) {
			Position newPos(command.x, command.y, command.z);
			if (player->getPosition() != newPos && newPos.x != 0) {
				g_game().internalTeleport(player, newPos, true);
			}
		}
	}
	
	const uint16_t count = static_cast<uint16_t>(presenceTable.size());
	std::vector<uint8_t> buf(3 + static_cast<size_t>(count) * 7, 0);
	buf[0] = S2C_USER_PRESENCE;
	writeU16LE_v(buf, 1, count);
	
	size_t offset = 3;
	for (const auto& [id, entry] : presenceTable) {
		writeU16LE_v(buf, offset, entry.userId);
		writeU16LE_v(buf, offset + 2, entry.x);
		writeU16LE_v(buf, offset + 4, entry.y);
		buf[offset + 6] = entry.z;
		offset += 7;
	}
	
	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildPresence(++utpSequence, myId, command.x, command.y, command.z));
}

void MV3DEditorBridge::applyEntityTrigger(const std::weak_ptr<asio::ip::tcp::socket> &/*client*/, EntityTrigger command) {
	if (mv3dPlayerId == 0) {
		logger.warn("MV3D editor bridge rejected 0xAC trigger UID {} because MV3D_PLAYER_ID is not set", command.uniqueId);
		return;
	}

	const auto player = g_game().getPlayerByID(mv3dPlayerId);
	if (!player) {
		logger.warn("MV3D editor bridge rejected 0xAC trigger UID {}: player {} not found", command.uniqueId, mv3dPlayerId);
		return;
	}

	const auto item = g_game().getUniqueItem(command.uniqueId);
	if (!item) {
		logger.warn("MV3D editor bridge rejected 0xAC trigger: unique item {} not found", command.uniqueId);
		return;
	}

	Position pos = item->getPosition();
	uint8_t stackPos = 0; // Se resolverá internamente o es irrelevante para unique items
	
	BLOG("MV3D bridge apply 0xAC trigger UID {} (item {}) via player {} @ {}", 
		command.uniqueId, item->getID(), player->getName(), pos.toString());

	// En Canary, playerUseItem maneja la lógica de palancas, puertas, etc.
	g_game().playerUseItem(player->getID(), pos, 255, 0, item->getID());
}

// =============================================================================
// BROADCASTS S→C — llamados por sistemas Canary (thread-safe)
// =============================================================================

void MV3DEditorBridge::broadcastDamageEvent(uint32_t targetId, int damage, uint8_t type) {
	std::vector<uint8_t> buf(10, 0);
	buf[0] = S2C_DAMAGE_EVENT;
	writeU32LE_v(buf, 1, targetId);
	writeI32LE_v(buf, 5, damage);
	buf[9] = type;
	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildDamage(++utpSequence, targetId, damage, type), true);
	BLOG("MV3D bridge TX 0xB1 damage target={} dmg={} type={}", targetId, damage, static_cast<int>(type));
}

void MV3DEditorBridge::broadcastEntityDeath(uint32_t targetId) {
	std::vector<uint8_t> buf(5, 0);
	buf[0] = S2C_ENTITY_DEATH;
	writeU32LE_v(buf, 1, targetId);
	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildDeath(targetId), true);
	BLOG("MV3D bridge TX 0xB2 death target={}", targetId);
}

void MV3DEditorBridge::broadcastInventoryDelta(uint32_t containerId, uint32_t itemId,
                                                uint16_t newQuantity, uint16_t amountApplied,
                                                bool deleteItem) {
	std::vector<uint8_t> buf(14, 0);
	buf[0] = S2C_INVENTORY_DELTA;
	writeU32LE_v(buf, 1, containerId);
	writeU32LE_v(buf, 5, itemId);
	buf[9]  = static_cast<uint8_t>(newQuantity & 0xFF);
	buf[10] = static_cast<uint8_t>((newQuantity >> 8) & 0xFF);
	buf[11] = static_cast<uint8_t>(amountApplied & 0xFF);
	buf[12] = static_cast<uint8_t>((amountApplied >> 8) & 0xFF);
	buf[13] = deleteItem ? 1 : 0;
	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildInventory(containerId, itemId, newQuantity, deleteItem));
	BLOG("MV3D bridge TX 0xB5 inventory delta container={} item={} qty={} del={}",
		containerId, itemId, newQuantity, deleteItem);
}

void MV3DEditorBridge::broadcastDialogState(uint32_t activeNodeId, bool closeDialog) {
	static uint32_t lastNode = 0xFFFFFFFF;
	static bool lastClose = false;
	
	if (activeNodeId == lastNode && closeDialog == lastClose) {
		return; // Ignorar spam de duplicados
	}
	lastNode = activeNodeId;
	lastClose = closeDialog;

	std::vector<uint8_t> buf(6, 0);
	buf[0] = S2C_DIALOG_STATE;
	writeU32LE_v(buf, 1, activeNodeId);
	buf[5] = closeDialog ? 1 : 0;
	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildDialog(activeNodeId, closeDialog));
	BLOG("MV3D bridge TX 0xB7 dialog state node={} close={}", activeNodeId, closeDialog);
}

void MV3DEditorBridge::broadcastWorldFlagDelta(const std::string &key, bool value) {
	const auto keyLen = static_cast<uint16_t>(key.size());
	std::vector<uint8_t> buf(4 + keyLen, 0);
	buf[0] = S2C_WORLD_FLAG_DELTA;
	buf[1] = static_cast<uint8_t>(keyLen & 0xFF);
	buf[2] = static_cast<uint8_t>((keyLen >> 8) & 0xFF);
	std::copy(key.begin(), key.end(), buf.begin() + 3);
	buf[3 + keyLen] = value ? 1 : 0;
	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildWorldFlag(key, value));
	BLOG("MV3D bridge TX 0xB8 world flag {}={}", key, value);
}

void MV3DEditorBridge::broadcastAiBehaviorDelta(uint32_t entityId, int16_t tileX, int16_t tileY,
                                                 uint8_t floor, uint8_t state) {
	std::vector<uint8_t> buf(11, 0);
	buf[0] = S2C_AI_BEHAVIOR_DELTA;
	writeU32LE_v(buf, 1, entityId);
	writeI16LE_v(buf, 5, tileX);
	writeI16LE_v(buf, 7, tileY);
	buf[9]  = floor;
	buf[10] = state;
	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildAiDelta(entityId, tileX, tileY, floor, state));
	BLOG("MV3D bridge TX 0xBA AI delta entity={} pos={},{},{} state={}",
		entityId, tileX, tileY, static_cast<int>(floor), static_cast<int>(state));
}

void MV3DEditorBridge::broadcastChatMessage(uint16_t senderId, const std::string &message) {
	uint16_t len = static_cast<uint16_t>(message.length());
	// opcode(1) + senderId(2) + strLen(2) + chars(len)
	std::vector<uint8_t> buf(5 + len, 0);
	buf[0] = S2C_CHAT_BROADCAST;
	writeU16LE_v(buf, 1, senderId);
	writeU16LE_v(buf, 3, len);
	std::memcpy(buf.data() + 5, message.data(), len);

	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildChat(++utpSequence, senderId, message));
}

void MV3DEditorBridge::broadcastBossPhaseSync(uint32_t entityId, uint8_t phase, int32_t hp) {
	std::vector<uint8_t> buf(10, 0);
	buf[0] = S2C_BOSS_PHASE_SYNC;
	writeU32LE_v(buf, 1, entityId);
	buf[5] = phase;
	writeI32LE_v(buf, 6, hp);
	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildBossPhase(entityId, phase, hp));
	BLOG("MV3D bridge TX 0xBC boss phase entity={} phase={} hp={}", entityId, static_cast<int>(phase), hp);
}

void MV3DEditorBridge::broadcastWorldLight(uint8_t level, uint8_t color) {
	// opcode(1) + level(1) + color(1) = 3
	std::vector<uint8_t> buf(3, 0);
	buf[0] = S2C_WORLD_LIGHT;
	buf[1] = level;
	buf[2] = color;
	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildWorldLight(level, color));
	BLOG("MV3D bridge TX 0xBD world light level={} color={}", static_cast<int>(level), static_cast<int>(color));
}

void MV3DEditorBridge::broadcastWeatherSync(uint8_t type, uint16_t intensity) {
	// opcode(1) + type(1) + intensity(2) = 4
	std::vector<uint8_t> buf(4, 0);
	buf[0] = S2C_WEATHER_SYNC;
	buf[1] = type;
	buf[2] = static_cast<uint8_t>(intensity & 0xFF);
	buf[3] = static_cast<uint8_t>((intensity >> 8) & 0xFF);
	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildWeather(type, intensity));
	BLOG("MV3D bridge TX 0xBE weather sync type={} intensity={}", static_cast<int>(type), intensity);
}

void MV3DEditorBridge::broadcastSoundEvent(uint16_t soundId, uint16_t x, uint16_t y, uint8_t z, uint8_t volume) {
	// opcode(1) + soundId(2) + x(2) + y(2) + z(1) + volume(1) = 9
	std::vector<uint8_t> buf(9, 0);
	buf[0] = S2C_SOUND_EVENT;
	writeU16LE_v(buf, 1, soundId);
	writeU16LE_v(buf, 3, x);
	writeU16LE_v(buf, 5, y);
	buf[7] = z;
	buf[8] = volume;
	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildSound(soundId, x, y, z, volume));
	BLOG("MV3D bridge TX 0xBF sound={} @ {},{},{} vol={}", soundId, x, y, static_cast<int>(z), static_cast<int>(volume));
}

void MV3DEditorBridge::broadcastEntityState(uint16_t uniqueId, uint8_t stateFlags, float lightIntensity, uint32_t expiresAtMs) {
	// opcode(1) + uniqueId(2) + stateFlags(1) + lightIntensity(4) + expiresAtMs(4) = 12
	std::vector<uint8_t> buf(12, 0);
	buf[0] = S2C_ENTITY_STATE;
	writeU16LE_v(buf, 1, uniqueId);
	buf[3] = stateFlags;
	writeF32LE_v(buf, 4, lightIntensity);
	writeU32LE_v(buf, 8, expiresAtMs);
	broadcastRaw(buf);
	broadcastUTP(mv3d::utp::BuildEntityState(uniqueId, stateFlags, lightIntensity, expiresAtMs));
	BLOG("MV3D bridge TX 0xAD entity UID {} state 0x{:02X} light {} expires {}ms", 
		uniqueId, static_cast<int>(stateFlags), lightIntensity, expiresAtMs);
}

// ── UTP Chunk streaming ───────────────────────────────────────────────────────

std::string MV3DEditorBridge::buildChunkJson(const std::vector<SnapshotEntry> &entries,
                                              const SyncRequest &req,
                                              int playerX, int playerY, int playerZ) {
    // Deduplica por posición: último entry = ítem más visible (el loop en applySyncRequest
    // va ground primero → items de abajo a arriba, por lo que el último es el topmost).
    std::unordered_map<uint64_t, const SnapshotEntry*> topItems;
    topItems.reserve(entries.size());
    for (const auto &e : entries) {
        const uint64_t key = (static_cast<uint64_t>(e.x) << 32)
                           | (static_cast<uint64_t>(e.y) << 16)
                           | e.z;
        topItems[key] = &e;
    }

    std::string tilesJson;
    try {
        tilesJson.reserve(topItems.size() * 80);
        bool first = true;
        size_t count = 0;
        for (const auto &kv : topItems) {
            if (++count > 5000) break; // Límite de seguridad por chunk

            const SnapshotEntry* entry = kv.second;
            const CatalogEntry* cat = utpCatalogLookup(entry->itemId);
            const char* shape   = cat ? cat->shape   : "floor";
            const char* type    = cat ? cat->type    : "void";
            const char* feature = cat ? cat->feature : "";
            if (!first) tilesJson += ',';
            tilesJson += mv3d::utp::BuildTile(entry->x, entry->y, entry->z,
                                              shape, type, feature, entry->itemId);
            first = false;
        }

        const uint16_t chunkX = static_cast<uint16_t>(req.x / 32);
        const uint16_t chunkY = static_cast<uint16_t>(req.y / 32);
        const uint32_t seq    = ++utpChunkSeq;
        return mv3d::utp::BuildChunk(chunkX, chunkY, seq, playerX, playerY, playerZ, tilesJson);
    } catch (const std::exception &e) {
        logger.error("[UTP] Fallo en buildChunkJson: {}", e.what());
        return "{}";
    }
}

void MV3DEditorBridge::broadcastChunk(const std::vector<SnapshotEntry> &entries,
                                       const SyncRequest &req,
                                       int playerX, int playerY, int playerZ) {
    if (entries.empty()) return;
    {
        std::scoped_lock lock(utpMutex);
        if (utpSubscribers.empty()) return;
    }
    broadcastUTP(buildChunkJson(entries, req, playerX, playerY, playerZ));
    BLOG("MV3D bridge UTP chunk seq={} tiles={} player={},{},{}",
         utpChunkSeq.load(), entries.size(), playerX, playerY, playerZ);
}

void MV3DEditorBridge::sendLockDenied(const std::shared_ptr<asio::ip::tcp::socket> &client, uint16_t x, uint16_t y, uint8_t z, uint16_t retryAfterMs, uint16_t ownerUserId) {
	if (!client) return;
	// opcode(1) + x(2) + y(2) + z(1) + retry(2) + owner(2) = 10
	std::vector<uint8_t> buf(10, 0);
	buf[0] = S2C_LOCK_DENIED;
	writeU16LE_v(buf, 1, x);
	writeU16LE_v(buf, 3, y);
	buf[5] = z;
	writeU16LE_v(buf, 6, retryAfterMs);
	writeU16LE_v(buf, 8, ownerUserId);

	std::scoped_lock lock(socketMutex);
	if (!client->is_open()) return;
	asio::error_code ec;
	asio::write(*client, asio::buffer(buf), ec);
}
