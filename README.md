# Canary Bridge 3D

Secure MV3D/UTP bridge for Canary with config-gated player operations and protected map editing.

This repository is a Canary-based experimental branch focused on a local 3D bridge for direct client testing, MV3D synchronization, UTP event streaming, and controlled gameplay opcode integration.

## What This Adds

- Local MV3D editor bridge over TCP.
- UTP event streaming for 3D clients and external visual runtimes.
- RSA-based bridge handshake for the local bridge channel.
- Player-bound opcode support for controlled direct-client testing.
- Separate configuration gates for player operations and map editing.

## Safety Model

The bridge is not intended to be an open exploitation tunnel.

Sensitive bridge features are controlled from `config.lua`:

```lua
mv3dEditorBridgeEnabled = true
mv3dBridgePlayerOperationMode = false
mv3dBridgeMapEditMode = false
```

`mv3dBridgePlayerOperationMode` gates player-bound opcodes such as attack, item use, NPC dialog, chat/listener updates, and related gameplay operation hooks.

`mv3dBridgeMapEditMode` gates map editing, map saving, metadata/text updates, and map snapshot streaming.

Recommended default for public or shared testing:

```lua
mv3dEditorBridgeEnabled = true
mv3dBridgePlayerOperationMode = false
mv3dBridgeMapEditMode = false
```

Enable player operation only for local controlled tests:

```lua
mv3dBridgePlayerOperationMode = true
mv3dBridgeMapEditMode = false
```

Enable map editing only in a trusted local editor environment:

```lua
mv3dBridgeMapEditMode = true
```

## Build

This repository contains a source overlay, not the full Canary build tree. Apply
the overlay to a compatible complete Canary checkout before configuring/building
the server with its dependencies. Existing local build directories may contain
absolute paths from another machine and must be reconfigured.

Sector streaming (`0xA5`–`0xA8`), the exact packet layout, permissions, client
coexistence limits and standalone codec test instructions are documented in
[MV3D_SECTOR_PROTOCOL.md](docs/MV3D_SECTOR_PROTOCOL.md).

The regular Tibia protocol and older bridge snapshot packet formats are unchanged.
The sector extension requires a matching client parser and live integration tests.

The preliminary bridge source release and local integration evidence are
described in [RELEASE_BRIDGE_0.1.0.md](docs/RELEASE_BRIDGE_0.1.0.md).
The SDL2 client is excluded and remains under testing.

Run the standalone wire codec tests without a server or SDL2:

```powershell
g++ -std=c++17 -Wall -Wextra -Werror -I src tests/mv3d_sector_protocol_test.cpp -o sector-test.exe
.\sector-test.exe
```

## External Contributions

Public contributions are welcome through pull requests, but all changes require maintainer review before merging.

Security-sensitive bridge, player, map-editing, networking, RSA, and config code requires explicit owner approval.

Please do not open pull requests that:

- Remove or bypass config gates.
- Enable map editing by default.
- Add item creation, monster summon, teleport, privilege escalation, or admin-style behavior through bridge opcodes.
- Expose the bridge beyond local/trusted development use without a security review.

## Branch Protection Recommendation

For this repository, keep `main` protected:

- Require pull requests before merging.
- Require at least one approval.
- Require conversations to be resolved.
- Do not give direct write access except to trusted maintainers.

## Upstream

This work is based on OpenTibiaBR Canary:

https://github.com/opentibiabr/canary
