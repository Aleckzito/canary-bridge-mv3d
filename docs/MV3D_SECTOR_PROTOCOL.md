# MV3D sector streaming (TCP)

This repository is a source overlay for Canary, not a complete standalone server.
The implementation lives on `canary-bridge-3d`; `main` currently contains only an
introductory README. Target bridge pull requests at `canary-bridge-3d`.

## Connection and permissions

Connect to the existing local TCP bridge (default `127.0.0.1:7173`) and complete
its existing handshake before sending opcodes. Sector snapshots use the same
`mv3dBridgeMapEditMode` gate as the older `0x93` snapshot request. This change does
not bypass that gate or enable editing by default. With the gate off, a sector
request closes the bridge connection. For trusted local sector tests:

```lua
mv3dEditorBridgeEnabled = true
mv3dBridgeMapEditMode = true
mv3dBridgePlayerOperationMode = false
```

Enabling this gate also permits editor operations. A viewer-only permission is
not implemented. The old manual's `mvBridgeEnabled` / `mvBridgeMapEditMode` names
are not the config keys loaded by this branch.

## Wire format

All integers are little-endian. Sizes below **include the opcode**. TCP is a byte
stream: use exact reads / buffered parsing; a socket read is not a packet.

| Packet | Fields in wire order | Size |
| --- | --- | --- |
| `0xA5` request | opcode:u8, x:u16, y:u16, z:u8, previewRadius:u8, sectorSizeTiles:u8 | 8 bytes |
| `0xA6` begin | opcode:u8, sectorId:u64, totalBytes:u32, chunkCount:u16, channel:u8, floor:u8, centerX:i32, centerY:i32, sectorSizeTiles:u8 | 26 bytes |
| `0xA7` data | opcode:u8, sectorId:u64, chunkIndex:u16, channel:u8, compression:u8, payloadLen:u16, payload | 15 + payloadLen |
| `0xA8` end | opcode:u8, sectorId:u64, channel:u8, status:u8 | 11 bytes |

- The `0xA5` payload is 7 bytes; the opcode makes the total 8.
- `sectorId = (uint64(x) << 32) | (uint64(y) << 16) | z`.
- Channel and compression are zero. Chunk indices start at zero.
- Each payload entry is `x:u16, y:u16, z:u8, itemId:u16` (7 bytes).
  There is no leading entry count. Ground and stacked items may repeat a position.
- Each data packet contains at most **197 entries**, keeping it below 1400 bytes.
- `totalBytes` counts payload bytes only; `chunkCount` counts data packets only.
- Radius is `clamp(previewRadius != 0 ? previewRadius : sectorSizeTiles / 2, 1, 16)`.
  The square includes both endpoints; coordinates clamp to `[0, 65535]`.
  The begin packet reports the nominal square width `2 * radius + 1`, before
  clipping at map boundaries, rather than the old hardcoded width 64.
- Floors are 0–15. Status is 0=complete, 1=truncated at 65535 entries,
  2=invalid floor. Empty/invalid sectors still return begin/end with zero chunks.
- A client must discard or retry incomplete sectors. This is a one-shot snapshot,
  not a persistent subscription or continuous position stream.

The old TODO's 8-byte BEGIN / 1-byte END description is incompatible with this
format. Update clients to the documented sizes before testing. Unknown opcodes
now close the connection because their payload length cannot be recovered safely.

## Coexistence and limits

The regular Tibia protocol and the older `0x93`/`0x94` bridge snapshot keep their
existing formats. Sector reads do not mutate the world. SDL2 and the raycaster
remain client responsibilities: classify item IDs before treating them as walls.

Both clients still share Canary's world and dispatcher. Editor/gameplay commands
can affect other players. Like existing bridge writes, sector writes are
synchronous under the shared socket mutex; a stalled client can delay server work.
This patch is for bounded local experiments, not a claim of production isolation.
The SDL2 client and simultaneous live client behavior have not been validated.

## Validation

The wire codec has no Canary/SDL2 dependencies:

```powershell
g++ -std=c++17 -Wall -Wextra -Werror -I src tests/mv3d_sector_protocol_test.cpp -o sector-test.exe
.\sector-test.exe
```

Tests cover golden packet bytes, empty sectors, radius normalization, 197/198
entry chunk boundaries, maximum coordinates, truncation and invalid-floor status.
A full server build and handshake/network smoke test require a complete Canary
checkout with its build dependencies and data; they are not supplied by this overlay.

For this patch, the bridge translation unit also passed an MSVC `/Zs` syntax check
using headers and vcpkg dependencies from the local full Canary checkout. This
checks compilation of the changed code, but does not link or run the server.
