# MatchMakingSvr

1:1 매치메이킹 서버 프로토타입입니다.
호스트는 4자리 코드로 방을 만들고, 게스트는 해당 코드로 참가합니다.
서버는 UDP hole punching 기반의 직접 연결을 시도하고, 실패하면 UDP relay fallback으로 통신을 이어줍니다.

Windows C++20 matchmaking prototype for 1:1 peer connection setup.

This repository contains:

- `MatchMakingSvr`: matchmaking server
- `MatchMakingDummyCli`: dummy client for local testing
- `Common`: shared protocol and utility code

The server supports:

- 4-digit room code generation
- host/guest room matching
- HTTP + JSON control API
- UDP registration and endpoint exchange
- UDP hole punching for direct P2P
- UDP relay fallback when direct punch fails

## Solution Layout

```text
WindUpSvr.sln
MatchMakingSvr/
MatchMakingDummyCli/
Common/
```

## Requirements

- Windows
- Visual Studio 2022
- MSVC v143
- C++20

## Build

Open `WindUpSvr.sln` in Visual Studio and build `Debug|x64` or `Release|x64`.

You can also build from terminal:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" WindUpSvr.sln /t:Build /p:Configuration=Debug /p:Platform=x64
```

## Run

Start the server first:

```powershell
.\x64\Debug\MatchMakingSvr.exe 8080 9000
```

- first argument: HTTP port
- second argument: UDP port

Run a host dummy client:

```powershell
.\x64\Debug\MatchMakingDummyCli.exe host 127.0.0.1 8080
```

The host prints a 4-digit room code.

Run a guest dummy client with that code:

```powershell
.\x64\Debug\MatchMakingDummyCli.exe guest 1234 127.0.0.1 8080
```

## Relay Fallback Test

To force relay mode instead of direct UDP punch:

```powershell
.\x64\Debug\MatchMakingDummyCli.exe host 127.0.0.1 8080 --force-relay
.\x64\Debug\MatchMakingDummyCli.exe guest 1234 127.0.0.1 8080 --force-relay
```

In normal flow:

1. both peers register UDP to the server
2. server sends peer endpoint info
3. clients try UDP hole punching
4. if direct traffic succeeds, session becomes `connected`
5. if punch times out, server switches to `relay`

## HTTP API

### `POST /rooms`

Creates a host room.

Response fields:

- `roomCode`
- `sessionToken`
- `udpServerPort`
- `expiresAt`
- `status`

### `POST /rooms/join`

Request body:

```json
{
  "roomCode": "1234"
}
```

Response fields:

- `roomCode`
- `sessionToken`
- `udpServerPort`
- `expiresAt`
- `status`

### `GET /rooms/{code}/status?sessionToken=...`

Returns current room/session state.

Possible states:

- `waiting_guest`
- `waiting_udp`
- `punching`
- `relay`
- `connected`
- `failed`
- `expired`

## UDP Control Messages

Client to server:

- `REGISTER <sessionToken>`
- `CONNECTED <sessionToken>`
- `RELAY <sessionToken> <payload>`

Server to client:

- `REGISTERED <ip> <port>`
- `PUNCH_START <peerIp> <peerPort>`
- `RELAY_READY <role>`
- `RELAY_FROM <role> <payload>`

## Unreal Integration Notes

The current protocol is intentionally simple for Unreal-side integration.

- Use Unreal HTTP for room creation/join/status polling
- Open a UDP socket on the client
- Send `REGISTER <token>` to the server UDP port
- Handle `PUNCH_START` for direct P2P attempts
- Handle `RELAY_READY` and `RELAY_FROM` for fallback relay mode

This is not a full TURN implementation.
It is a custom matchmaking server with built-in UDP relay fallback.

## Current Scope

- 1 host + 1 guest only
- in-memory room storage
- no persistence
- no authentication
- no encryption
- no standard TURN/STUN compatibility layer

## License

No license file is included yet.
