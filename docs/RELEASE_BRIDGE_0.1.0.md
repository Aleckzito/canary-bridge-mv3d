# Bridge 0.1.0 — preliminary source release

This release packages the Canary MV3D bridge source overlay, including the
sector streaming fix from commit `33acaaa`. It does not include the SDL2 client,
client assets, a standalone server executable, databases or local credentials.
Apply the overlay to a compatible complete Canary checkout and build it using
that checkout's normal dependencies and data. This is a preliminary development
release for local testing.

## Changes

- Handle sector request `0xA5` and emit matching `0xA6`/`0xA7`/`0xA8` packets.
- Serialize flat tile entries with bounded chunks, explicit completion status,
  normalized radius, coordinate limits and floors z=0 through z=15.
- Preserve the map-edit configuration gate for snapshots and close connections
  on unknown opcodes to avoid parsing an unknown payload as subsequent packets.
- Document exact packet lengths and retain the older bridge snapshot formats.

## Validation evidence — 2026-10-22

The standalone C++17 codec tests cover golden bytes, empty sectors, 197/198 entry
chunk boundaries, maximum coordinates, truncation and invalid-floor status.

The sector fix was integrated into a complete local Canary checkout, compiled
with MSVC x64, and exercised against its running bridge using a separate
experimental test client. A 16-floor run completed the RSA handshake and
received 144 sectors with 25183 entries without client stream errors or
incomplete transfers. Other runs exercised surface-only streaming and camera
positions in underground floors. Camera rendering and navigation belong to the
test client and are not released here.

The full local checkout contains other local changes; these checks establish
integration evidence for the sector extension, not a clean standalone build of
this overlay or certification of the entire server. A later repeat completed
the handshake but lost the connection when the server stopped; its shutdown
cause was not established. No claim of long-running stability is made.

## Configuration and limits

Keep the bridge in a trusted local environment. The distributed sample keeps
player-operation and map-edit gates disabled. Sector reads require explicitly
enabling `mv3dBridgeMapEditMode` in the local server configuration.

The Tibia protocol is unchanged, but regular-client coexistence has not been
tested. Synchronous bridge writes can delay shared server work. Authentication,
gameplay, world-edit operations and deployment security require further review.
The SDL2 client needs additional testing and is deliberately excluded.

The ZIP is an exact archive of the tagged overlay sources. `SHA256SUMS.txt`
records its SHA-256 digest. The original project remains based on OpenTibiaBR
Canary; this release does not introduce a new license grant.
