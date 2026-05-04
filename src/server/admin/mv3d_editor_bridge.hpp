/**
 * MV3D editor bridge - raw local TCP bridge for MV3D Hybrid Engine.
 *
 * ARQUITECTURA (ver MASTER_JOURNAL.md en C:\MV3D):
 *   [MV3D Client] <--TCP 7173--> [MV3DEditorBridge] <--> [Canary game systems]
 *
 * ESTADO DEL PROTOCOLO:
 *   0x90-0x9A  Editor de mapas — IMPLEMENTADO Y CERTIFICADO
 *   0x9B-0xAD  Colaboración, historial, sector streaming — PENDIENTE (ver EditorBridge.exe como ref)
 *   0xB0-0xBC  Gameplay (combate, inventario, IA, boss) — CIMIENTOS LISTOS, hooks pendientes
 *
 * BROADCASTS (S→C): llamados por sistemas Canary via broadcastXxx() — thread-safe.
 * HANDLERS  (C→S): procesados en runLoop(), despachados al main thread via g_dispatcher().
 *
 * TODO CODEX: conectar broadcastXxx() a los sistemas Canary correspondientes:
 *   broadcastDamageEvent   → combat.cpp  (onCreatureDamage callback)
 *   broadcastEntityDeath   → combat.cpp  (onCreatureDeath callback)
 *   broadcastAiBehavior    → monster.cpp (onCreatureMove / AI tick)
 *   broadcastBossPhaseSync → game.cpp    (boss phase transition event)
 *   broadcastInventoryDelta→ player.cpp  (onInventoryUpdate callback)
 *   broadcastDialogState   → npc.cpp     (onDialogNode callback)
 *   broadcastWorldFlagDelta→ game.cpp    (setWorldFlag hook)
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <asio.hpp>

class Logger;

namespace spdlog {
	class logger;
}

class MV3DEditorBridge {
public:
	explicit MV3DEditorBridge(Logger &logger);
	~MV3DEditorBridge();

	MV3DEditorBridge(const MV3DEditorBridge &) = delete;
	MV3DEditorBridge &operator=(const MV3DEditorBridge &) = delete;

	void start();
	void stop();

	bool isEnabled() const { return enabled; }
	uint16_t getPort() const { return port; }
	uint32_t getMV3DPlayerId() const { return mv3dPlayerId; }
	bool isMapEditModeEnabled() const { return mapEditMode; }
	bool isPlayerOperationModeEnabled() const { return playerOperationMode; }

	// -------------------------------------------------------------------------
	// BROADCAST API — llamada por sistemas Canary para pushear eventos al cliente
	// Todas son thread-safe (usan socketMutex internamente)
	// -------------------------------------------------------------------------

	/** combat.cpp → onCreatureDamage: notifica daño aplicado a una entidad */
	void broadcastDamageEvent(uint32_t targetId, int damage, uint8_t type);

	/** combat.cpp → onCreatureDeath: notifica muerte de una entidad */
	void broadcastEntityDeath(uint32_t targetId);

	/** player.cpp → onInventoryUpdate: delta incremental de inventario */
	void broadcastInventoryDelta(uint32_t containerId, uint32_t itemId,
	                             uint16_t newQuantity, uint16_t amountApplied,
	                             bool deleteItem);

	/** npc.cpp → onDialogNode: avanza o cierra el árbol de diálogo activo */
	void broadcastDialogState(uint32_t activeNodeId, bool closeDialog);

	/** game.cpp → setWorldFlag hook: propaga flag global al cliente */
	void broadcastWorldFlagDelta(const std::string &key, bool value);

	/** monster.cpp / AIController → tick: replica posición y estado IA */
	void broadcastAiBehaviorDelta(uint32_t entityId, int16_t tileX, int16_t tileY,
	                              uint8_t floor, uint8_t state);

	/** game.cpp → boss phase transition: sincroniza fase y HP de boss */
	void broadcastBossPhaseSync(uint32_t entityId, uint8_t phase, int32_t hp);

	/** bridge → chat relay: envía mensaje a todos los suscriptores */
	void broadcastChatMessage(uint16_t senderId, const std::string &message);

	/** game.cpp → checkLight: sincroniza luz ambiental global */
	void broadcastWorldLight(uint8_t level, uint8_t color);

	/** game.cpp → weather change: sincroniza lluvia/nieve/etc */
	void broadcastWeatherSync(uint8_t type, uint16_t intensity);

	/** world → sound trigger: dispara sonido posicional en todos los clientes */
	void broadcastSoundEvent(uint16_t soundId, uint16_t x, uint16_t y, uint8_t z, uint8_t volume);

	/** world → entity state: replica estado (on/off, open/closed) de un objeto interactivo */
	void broadcastEntityState(uint16_t uniqueId, uint8_t stateFlags, float lightIntensity, uint32_t expiresAtMs);

private:
	// -------------------------------------------------------------------------
	// Structs de comandos C→S (entrantes del cliente)
	// -------------------------------------------------------------------------
	struct PaintCommand {
		uint16_t x = 0, y = 0;
		uint8_t z = 0;
		uint16_t itemId = 0;
	};
	struct EraseCommand {
		uint16_t x = 0, y = 0;
		uint8_t z = 0;
		uint16_t itemId = 0;
	};
	struct EraseByStackCommand {
		uint16_t x = 0, y = 0;
		uint8_t z = 0;
		uint16_t itemId = 0;
		uint8_t stackIndex = 0;
	};
	struct MetadataUpdateCommand {
		uint16_t x = 0, y = 0;
		uint8_t z = 0;
		uint16_t itemId = 0;
		uint8_t stackIndex = 0;
		uint16_t actionId = 0;
		uint16_t uniqueId = 0;
		uint32_t transactionId = 0;
	};
	struct TextUpdateCommand {
		uint16_t x = 0, y = 0;
		uint8_t z = 0;
		uint16_t itemId = 0;
		uint8_t stackIndex = 0;
		uint32_t transactionId = 0;
		std::string text;
	};
	struct SyncRequest {
		uint16_t x = 0, y = 0;
		uint8_t z = 0, range = 0;
	};
	struct SnapshotEntry {
		uint16_t x = 0, y = 0;
		uint8_t z = 0;
		uint16_t itemId = 0;
	};

	// Gameplay C→S
	struct AttackCommand {
		uint32_t targetId = 0;
	};
	struct ItemUseCommand {
		uint32_t containerId = 0;
		uint32_t itemId = 0;
	};
	struct DialogChoiceCommand {
		uint32_t nodeId = 0;
		uint8_t optionIndex = 0;
	};
	struct ChatMessageCommand {
		std::string message;
	};
	struct AiBehaviorReport {
		uint32_t entityId = 0;
		int16_t tileX = 0, tileY = 0;
		uint8_t floor = 0, state = 0;
	};
	struct ListenerUpdateCommand {
		float posX = 0, posY = 0, posZ = 0;
		float dirX = 0, dirY = 0, dirZ = 0;
	};
	struct BossPhaseReport {
		uint32_t entityId = 0;
		uint8_t phase = 1;
		int32_t hp = 0;
	};

	// Colaboración
	struct LockRequest {
		uint16_t x = 0, y = 0;
		uint8_t z = 0;
		uint8_t lockMode = 0; // 0=acquire, 1=release, 2=force
		uint16_t leaseMs = 0;
	};
	struct PresenceUpdate {
		uint16_t x = 0, y = 0;
		uint8_t z = 0;
	};
	struct EntityTrigger {
		uint16_t uniqueId = 0;
		uint8_t triggerType = 0;
		uint16_t paramMs = 0;
	};

	struct LockInfo {
		uint32_t ownerId = 0;
		std::chrono::steady_clock::time_point expiresAt;
	};
	struct PresenceEntry {
		uint16_t userId = 0;
		uint16_t x = 0, y = 0;
		uint8_t z = 0;
	};

	// -------------------------------------------------------------------------
	// Opcodes — Editor de mapas (0x90-0x9A) IMPLEMENTADOS
	// -------------------------------------------------------------------------
	static constexpr uint8_t C2S_MAP_EDIT_PAINT        = 0x90;
	static constexpr uint8_t C2S_MAP_EDIT_ERASE        = 0x91;
	static constexpr uint8_t S2C_MAP_SYNC_UPDATE       = 0x92;
	static constexpr uint8_t C2S_MAP_SYNC_REQUEST      = 0x93;
	static constexpr uint8_t S2C_MAP_SYNC_SNAPSHOT     = 0x94;
	static constexpr uint8_t C2S_ERASE_BY_STACK        = 0x95;
	static constexpr uint8_t C2S_MAP_SAVE_REQUEST      = 0x96;
	static constexpr uint8_t S2C_MAP_SAVE_STATUS       = 0x97;
	static constexpr uint8_t C2S_ITEM_UPDATE_METADATA  = 0x98;
	static constexpr uint8_t C2S_ITEM_UPDATE_TEXT      = 0x99;
	static constexpr uint8_t S2C_EDIT_ACK              = 0x9A;

	// Opcodes — Colaboración e Historia (0x9B-0xAD)
	static constexpr uint8_t C2S_LOCK_REQUEST          = 0x9B;
	static constexpr uint8_t S2C_LOCK_DENIED           = 0x9C;
	static constexpr uint8_t S2C_USER_PRESENCE         = 0x9D;
	static constexpr uint8_t C2S_USER_PRESENCE_UPDATE  = 0x9E;
	static constexpr uint8_t C2S_CHAT_MESSAGE          = 0x9F;
	static constexpr uint8_t S2C_CHAT_BROADCAST        = 0xA0;
	static constexpr uint8_t C2S_HISTORY_QUERY         = 0xA1;
	static constexpr uint8_t S2C_HISTORY_DATA          = 0xA2;
	static constexpr uint8_t C2S_HISTORY_REVERT        = 0xA3;
	static constexpr uint8_t S2C_HISTORY_REVERT_RESULT = 0xA4;
	static constexpr uint8_t C2S_SECTOR_SUBSCRIBE      = 0xA5;
	static constexpr uint8_t S2C_SECTOR_CHUNK_BEGIN    = 0xA6;
	static constexpr uint8_t S2C_SECTOR_CHUNK_DATA     = 0xA7;
	static constexpr uint8_t S2C_SECTOR_CHUNK_END      = 0xA8;
	static constexpr uint8_t C2S_COMMIT_QUEUE_QUERY    = 0xA9;
	static constexpr uint8_t S2C_COMMIT_QUEUE_STATUS   = 0xAA;
	static constexpr uint8_t C2S_SECTOR_UNSUBSCRIBE    = 0xAB;
	static constexpr uint8_t C2S_ENTITY_TRIGGER        = 0xAC;
	static constexpr uint8_t S2C_ENTITY_STATE          = 0xAD;

	// Opcodes — Gameplay (0xB0-0xBC) CIMIENTOS LISTOS
	static constexpr uint8_t C2S_ATTACK_TARGET         = 0xB0;
	static constexpr uint8_t S2C_DAMAGE_EVENT          = 0xB1;
	static constexpr uint8_t S2C_ENTITY_DEATH          = 0xB2;
	static constexpr uint8_t C2S_ITEM_USE_REQUEST      = 0xB3;
	static constexpr uint8_t S2C_ITEM_USE_RESULT       = 0xB4;
	static constexpr uint8_t S2C_INVENTORY_DELTA       = 0xB5;
	static constexpr uint8_t C2S_DIALOG_CHOICE         = 0xB6;
	static constexpr uint8_t S2C_DIALOG_STATE          = 0xB7;
	static constexpr uint8_t S2C_WORLD_FLAG_DELTA      = 0xB8;
	static constexpr uint8_t C2S_AI_BEHAVIOR_REPORT    = 0xB9;
	static constexpr uint8_t S2C_AI_BEHAVIOR_DELTA     = 0xBA;
	static constexpr uint8_t C2S_BOSS_PHASE_REPORT     = 0xBB;
	static constexpr uint8_t S2C_BOSS_PHASE_SYNC       = 0xBC;
	static constexpr uint8_t S2C_WORLD_LIGHT           = 0xBD;
	static constexpr uint8_t S2C_WEATHER_SYNC          = 0xBE;
	static constexpr uint8_t S2C_SOUND_EVENT           = 0xBF;
	static constexpr uint8_t C2S_LISTENER_UPDATE       = 0xC0;

	// -------------------------------------------------------------------------
	// Tamaños de payload (bytes después del opcode)
	// -------------------------------------------------------------------------
	static constexpr size_t PAINT_PAYLOAD_SIZE          = 7;
	static constexpr size_t ERASE_PAYLOAD_SIZE          = 7;
	static constexpr size_t ERASE_BY_STACK_PAYLOAD_SIZE = 8;
	static constexpr size_t SYNC_REQUEST_PAYLOAD_SIZE   = 6;
	static constexpr size_t SYNC_CONFIRMATION_SIZE      = 8;
	static constexpr size_t EDIT_ACK_SIZE               = 7;
	static constexpr size_t METADATA_PAYLOAD_SIZE       = 16;
	static constexpr size_t TEXT_HEADER_PAYLOAD_SIZE    = 12;
	static constexpr size_t SAVE_STATUS_SIZE            = 2;

	// Colaboración
	static constexpr size_t LOCK_REQUEST_PAYLOAD_SIZE   = 8;
	static constexpr size_t PRESENCE_UPDATE_PAYLOAD_SIZE = 5;
	static constexpr size_t ENTITY_TRIGGER_PAYLOAD_SIZE = 5;

	// Gameplay
	static constexpr size_t ATTACK_PAYLOAD_SIZE         = 4;  // targetId(4)
	static constexpr size_t ITEM_USE_PAYLOAD_SIZE       = 8;  // containerId(4) + itemId(4)
	static constexpr size_t DIALOG_CHOICE_PAYLOAD_SIZE  = 5;  // nodeId(4) + optionIndex(1)
	static constexpr size_t AI_REPORT_PAYLOAD_SIZE      = 9;  // entityId(4)+tileX(2)+tileY(2)+floor(1)+state(1) = 10? Let me recount: entityId(4)+tileX(2)+tileY(2)+floor(1)+state(1)=10
	static constexpr size_t BOSS_REPORT_PAYLOAD_SIZE    = 9;  // entityId(4)+phase(1)+hp(4)

	// -------------------------------------------------------------------------
	// Handlers C→S (procesados en runLoop, despachados al main thread)
	// -------------------------------------------------------------------------
	void runLoop();
	bool performBridgeHandshake(const std::shared_ptr<asio::ip::tcp::socket> &client);
	void handleClient(std::shared_ptr<asio::ip::tcp::socket> client);
	bool isMapEditOpcode(uint8_t opcode) const;
	bool isPlayerOperationOpcode(uint8_t opcode) const;
	void runUTPOutboundLoop();
	void runUTPSubscriberLoop();
	void runUTPHeartbeatLoop();

	// Editor (implementados)
	void handlePaintPacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, PAINT_PAYLOAD_SIZE> &payload);
	void handleErasePacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, ERASE_PAYLOAD_SIZE> &payload);
	void handleEraseByStackPacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, ERASE_BY_STACK_PAYLOAD_SIZE> &payload);
	void handleSyncRequestPacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, SYNC_REQUEST_PAYLOAD_SIZE> &payload);
	void handleMetadataUpdatePacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, METADATA_PAYLOAD_SIZE> &payload);
	void handleTextUpdatePacket(const std::shared_ptr<asio::ip::tcp::socket> &client, TextUpdateCommand command);
	void handleSaveRequestPacket(const std::shared_ptr<asio::ip::tcp::socket> &client);

	// Colaboración
	void handleLockRequestPacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, LOCK_REQUEST_PAYLOAD_SIZE> &payload);
	void handlePresenceUpdatePacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, PRESENCE_UPDATE_PAYLOAD_SIZE> &payload);
	void handleEntityTriggerPacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, ENTITY_TRIGGER_PAYLOAD_SIZE> &payload);

	// Gameplay (stubs — Codex conecta al main thread via g_dispatcher)
	void handleAttackPacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, ATTACK_PAYLOAD_SIZE> &payload);
	void handleItemUsePacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, ITEM_USE_PAYLOAD_SIZE> &payload);
	void handleDialogChoicePacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, DIALOG_CHOICE_PAYLOAD_SIZE> &payload);
	void handleAiBehaviorReportPacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, 10> &payload);
	void handleBossPhaseReportPacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, BOSS_REPORT_PAYLOAD_SIZE> &payload);
	void handleChatMessagePacket(const std::shared_ptr<asio::ip::tcp::socket> &client);
	void handleListenerUpdatePacket(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::array<uint8_t, 24> &payload);

	// -------------------------------------------------------------------------
	// Apply (ejecutan en main thread via g_dispatcher)
	// -------------------------------------------------------------------------
	void applyPaintCommand(const std::weak_ptr<asio::ip::tcp::socket> &client, PaintCommand command);
	void applyEraseCommand(const std::weak_ptr<asio::ip::tcp::socket> &client, EraseCommand command);
	void applyEraseByStackCommand(const std::weak_ptr<asio::ip::tcp::socket> &client, EraseByStackCommand command);
	void applySyncRequest(const std::weak_ptr<asio::ip::tcp::socket> &client, SyncRequest request);
	void applyMetadataUpdate(const std::weak_ptr<asio::ip::tcp::socket> &client, MetadataUpdateCommand command);
	void applyTextUpdate(const std::weak_ptr<asio::ip::tcp::socket> &client, TextUpdateCommand command);
	void applySaveRequest(const std::weak_ptr<asio::ip::tcp::socket> &client);

	// Colaboración apply
	void applyLockRequest(const std::weak_ptr<asio::ip::tcp::socket> &client, LockRequest command);
	void applyPresenceUpdate(const std::weak_ptr<asio::ip::tcp::socket> &client, PresenceUpdate command);
	void applyEntityTrigger(const std::weak_ptr<asio::ip::tcp::socket> &client, EntityTrigger command);

	// Gameplay apply (TODO CODEX: implementar hooks a Canary)
	void applyAttackCommand(const std::weak_ptr<asio::ip::tcp::socket> &client, AttackCommand command);
	void applyItemUseCommand(const std::weak_ptr<asio::ip::tcp::socket> &client, ItemUseCommand command);
	void applyDialogChoiceCommand(const std::weak_ptr<asio::ip::tcp::socket> &client, DialogChoiceCommand command);
	void applyChatMessageCommand(const std::weak_ptr<asio::ip::tcp::socket> &client, ChatMessageCommand command);
	void applyListenerUpdateCommand(const std::weak_ptr<asio::ip::tcp::socket> &client, ListenerUpdateCommand command);

	// -------------------------------------------------------------------------
	// Send helpers (S→C) — todos requieren socket abierto
	// -------------------------------------------------------------------------
	void sendSyncConfirmation(const std::shared_ptr<asio::ip::tcp::socket> &client, const PaintCommand &command);
	void sendSnapshot(const std::shared_ptr<asio::ip::tcp::socket> &client, const std::vector<SnapshotEntry> &entries);
	void sendEditAck(const std::shared_ptr<asio::ip::tcp::socket> &client, uint32_t transactionId, uint8_t sourceOpcode, uint8_t statusCode);
	void sendSaveStatus(const std::shared_ptr<asio::ip::tcp::socket> &client, uint8_t statusCode);
	void sendItemUseResult(const std::shared_ptr<asio::ip::tcp::socket> &client, uint32_t containerId, uint32_t itemId, uint8_t approved, uint8_t reasonCode);
	void sendLockDenied(const std::shared_ptr<asio::ip::tcp::socket> &client, uint16_t x, uint16_t y, uint8_t z, uint16_t retryAfterMs, uint16_t ownerUserId);

	/** Envía bytes raw a todos los suscriptores. Thread-safe. */
	void broadcastRaw(const std::vector<uint8_t> &data);
	void broadcastRawSyncUpdate(const PaintCommand &command);

	// -------------------------------------------------------------------------
	// Persistencia
	// -------------------------------------------------------------------------
	bool didMapBinaryTimestampAdvance(const std::filesystem::path &mapPath, const std::filesystem::file_time_type &beforeWriteTime, std::filesystem::file_time_type &afterWriteTime) const;
	bool tryDeepBinarySaveViaCommand(const std::filesystem::path &mapPath) const;
	bool flushEditsToDisk();
	void loadEditsFromDisk();

	struct EditEntry {
		uint8_t op;
		uint16_t x = 0, y = 0;
		uint8_t z = 0;
		uint16_t itemId = 0;
	};

	// -------------------------------------------------------------------------
	// Estado del bridge
	// -------------------------------------------------------------------------
	Logger &logger;
	std::shared_ptr<spdlog::logger> bridgeFileLogger_;
	std::atomic_bool running = false;
	bool enabled = true;
	bool mapEditMode = false;
	bool playerOperationMode = false;
	uint16_t port = 7173;
	uint32_t mv3dPlayerId = 0;
	std::thread worker;
	std::thread utpOutboundWorker;
	std::thread utpSubscriberWorker;
	std::thread utpHeartbeatWorker;
	std::unique_ptr<asio::io_context> ioContext;
	std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
	std::mutex socketMutex;
	std::vector<std::shared_ptr<asio::ip::tcp::socket>> clients;
	std::unordered_map<std::shared_ptr<asio::ip::tcp::socket>, uint16_t> socketToUserId;
	std::vector<EditEntry> editLog;

	// Estado colaborativo
	std::unordered_map<uint64_t, LockInfo> lockTable;
	std::unordered_map<uint16_t, PresenceEntry> presenceTable;
	uint16_t nextUserId = 1;

	// UTP — UDP JSON universal para clientes D3D/LOVE/Godot/Unity/Unreal.
	std::unique_ptr<asio::ip::udp::socket> utpSocket;
	std::unique_ptr<asio::ip::udp::socket> utpSubscribeSocket;
	struct UtpSubscriber {
		asio::ip::udp::endpoint endpoint;
		std::chrono::steady_clock::time_point lastSeen;
	};
	std::unordered_map<std::string, UtpSubscriber> utpSubscribers;
	std::mutex utpMutex;
	std::mutex utpQueueMutex;
	std::condition_variable utpQueueCv;
	std::deque<std::string> utpHighPriorityQueue;
	std::deque<std::string> utpNormalPriorityQueue;
	uint16_t utpPort = 4242;
	uint16_t utpSubscribePort = 4243;
	size_t utpMaxQueue = 512;
	std::atomic<uint32_t> utpHeartbeatSeq = 0;
	std::atomic<uint32_t> utpFragmentSeq = 0;
	std::atomic<uint32_t> utpSequence = 0;
	std::atomic<uint32_t> utpChunkSeq = 0;

	void broadcastUTP(const std::string &json, bool highPriority = false);
	void enqueueUTPPacket(const std::string &json, bool highPriority);
	void registerUTPSubscriber(const asio::ip::udp::endpoint &ep);
	void pruneUTPSubscribers();

	/** Emite kind:"chunk" por UDP a todos los suscriptores.
	 *  Llama a broadcastUTP() internamente (soporta fragmentación automática). */
	void broadcastChunk(const std::vector<SnapshotEntry> &entries,
	                    const SyncRequest &req,
	                    int playerX, int playerY, int playerZ);
	std::string buildChunkJson(const std::vector<SnapshotEntry> &entries,
	                           const SyncRequest &req,
	                           int playerX, int playerY, int playerZ);
};
