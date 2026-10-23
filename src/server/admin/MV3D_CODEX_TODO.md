# MV3D Bridge — TODOs para Codex / Claude

## Estado actual
El bridge maneja correctamente 0x90-0x9A (editor de mapas), 0x9B-0xAD (colaboración e historia),
0xA5-0xA8 (sector streaming) y tiene los **cimientos completos** para 0xB0-0xBC (gameplay).
Los handlers parsean, logean y despachan al main thread.
Lo que falta es conectar los `apply*` y `broadcast*` a los sistemas reales de Canary.

---

## TODO 1 — Conectar broadcastDamageEvent a combat.cpp [DONE]
## TODO 2 — Conectar broadcastEntityDeath a combat.cpp [DONE]
## TODO 3 — Conectar broadcastAiBehaviorDelta a monster.cpp [DONE]
## TODO 4 — Implementar applyAttackCommand [DONE]
## TODO 5 — Implementar applyItemUseCommand [DONE]
## TODO 6 — Implementar applyDialogChoiceCommand [DONE]
## TODO 7 — Tracking del playerId del cliente MV3D [DONE]
## TODO 8 — Exponer getMV3DBridge() en CanaryServer [DONE]

## TODO 9 — Sincronización de World Flags (Global Storage) [DONE]
## TODO 10 — Sincronización de Diálogo NPC (broadcastDialogState) [DONE]
## TODO 11 — Sincronización de Inventario (broadcastInventoryDelta) [DONE]

---

## Estado de Sincronización (Fase 2) — COMPLETO

1. broadcastWorldFlagDelta -> Game::setWorldFlag (Lua expuesta) ✅
2. broadcastDialogState -> Npc::onDialogNode ✅
3. broadcastInventoryDelta -> Player::postAdd/postRemove (Offset 64) ✅
4. Atmospheric Sync (Luz, Clima, Sonido) -> Game hooks ✅

---

## TODO 12 — Implementar 0xA5 C2S_SECTOR_SUBSCRIBE (Fase 30.1) [DONE]

Implementado en sesión 2026-05-05 (Claude + Alex).

**Problema resuelto:** El bridge ignoraba el opcode 0xA5 sin consumir sus 7 bytes del buffer TCP,
desincronizando el stream completo (opcodes basura 0x07, 0x03, 0x40 a continuación).

**Fix aplicado:**
- `SectorSubscribe` struct + `SECTOR_SUBSCRIBE_PAYLOAD_SIZE = 7` en el HPP
- `handleSectorSubscribePacket` + `applySectorSubscribe` declarados en HPP e implementados en CPP
- Rama `else if (opcode == C2S_SECTOR_SUBSCRIBE)` añadida al dispatch loop
- Respuesta completa: `0xA6 BEGIN` (26 bytes) → N×`0xA7 DATA` (15 bytes de header + ≤197 entradas de 7 bytes) → `0xA8 END` (11 bytes)

**Contexto Fase 30.1:** Habilita que el Agente Sumergido reciba tiles del mapa para asignar
los +1000 sprites IA (ITA) y comenzar patrullaje. Prerequisito para la fase de Raycast.

**Bug del cliente corregido (Gemini + Alex, misma sesión):**
- `stbi_load` crash con sprites indexados/grayscale → forzar 4 canales RGBA
- `uint16_t` underflow en raycasting fuera de pantalla → clamp `[0, 65534]`

---

## Lo que YA está listo (no tocar)

- `runLoop()` — parsea y despacha 0x90-0x9A, 0x9B-0xAD, 0xA5, 0xB0-0xBC ✅
- Todos los `handle*Packet()` — leen payload y despachan al main thread ✅
- Todos los `broadcast*()` — serializan y envían al cliente ✅
- `sendRawToActiveClient()` — thread-safe ✅
- Helpers `writeU32LE_v`, `writeI32LE_v`, `writeI16LE_v` ✅
- `applySectorSubscribe()` — 0xA6/0xA7/0xA8 chunked TCP response ✅
