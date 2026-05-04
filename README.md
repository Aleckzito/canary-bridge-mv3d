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

Windows RelWithDebInfo build tested with:

```powershell
cmake --build "G:\Canary 3D\opentibiabr_repos\canary\build" --config RelWithDebInfo --target canary
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
