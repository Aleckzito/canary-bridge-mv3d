# MV3D Bridge — TODOs para Codex

## Estado actual
El bridge maneja correctamente 0x90-0x9A (editor de mapas) y tiene los **cimientos completos**
para 0xB0-0xBC (gameplay). Los handlers parsean, logean y despachan al main thread.
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

## Lo que YA está listo (no tocar)

- `runLoop()` — parsea y despacha 0x90-0x9A y 0xB0-0xBC ✅
- Todos los `handle*Packet()` — leen payload y despachan al main thread ✅
- Todos los `broadcast*()` — serializan y envían al cliente ✅
- `sendRawToActiveClient()` — thread-safe ✅
- Helpers `writeU32LE_v`, `writeI32LE_v`, `writeI16LE_v` ✅
