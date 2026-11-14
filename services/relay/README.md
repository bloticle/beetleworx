# Beetleworx internet relay

The relay is deployed at https://beetleworx-relay.ryanckoop1.workers.dev.
A live component check verified host/guest WebSockets, unchanged payloads in
both directions and rejection of new joins after a room starts. Native gameplay
over the relay and real Discord invitations still need end-to-end testing.

The current live deployment still carries gameplay/text only. The source also
contains a separate media endpoint, verified in the local Cloudflare runtime.
That media endpoint is not deployed. The native adapter compiles, but its
queue fixtures and actual encrypted/audio relay path have not run yet.
Demo.4 still cannot relay voice.

The service accepts outbound HTTPS/WebSocket connections from the host and up
to fifteen guests. Players do not open inbound ports or configure routers.
Cloudflare terminates TLS; game data is visible to the service, not end-to-end
encrypted. The existing game's handshake, save validation and tick proofs still
apply after relaying. The eight-character room code grants guest access: share
it only with intended participants. Host administration uses a separate secret.

## Deployment

Requires a Cloudflare account with Workers and Durable Objects enabled. Review
the account's billing and spending limits before enabling a public endpoint.
From this folder run `npx wrangler deploy` after signing into that account.
No relay helper executable is distributed to players. The native client uses
Windows HTTPS/WebSocket support inside `beetleworx.exe`. The distribution must
supply the deployed origin through `em2_online_relay_url`. With that configured,
the native lobby uses the Room backend and copied eight-character room codes;
the public EM1 preview remains offline. Without a deployed origin, the public
demo keeps online hidden.

The implementation follows Cloudflare's [WebSocket Hibernation API](https://developers.cloudflare.com/durable-objects/best-practices/websockets/).
Raw IP addresses, invite contents, host keys and game packets are not logged.
Per-address creation/lookup admission is bounded; daily hashed admission records
expire. Rooms expire after six hours. A host disconnect closes all guests.
This does not provide reconnect, public matchmaking or voice relaying.
The native [Discord integration](../../docs/ONLINE-DISCORD.md) supplies party
presence and accepted-invite handoff; it still needs live verification after
this service is deployed.

## Native integration contract

- `POST /v1/rooms` JSON `{version, invitation}` returns `{protocol:1, code,
  hostKey, version}`. The invitation is the existing native Room invitation.
- All room GET requests send `X-Beetleworx-Version`. Prereleases must match;
  SemVer build metadata is ignored. HTTP 409 reports `requiredVersion`.
- `GET /v1/rooms/CODE` obtains the version, underlying invitation and capacity
  only after the host's outbound WebSocket has connected.
- Host upgrades `/v1/rooms/CODE/host` using `Authorization: Bearer HOSTKEY`.
- Guests upgrade `/v1/rooms/CODE/join`. Capacity includes the host and is 16.
- Binary WebSocket messages start with kind and channel bytes. Kind 1 announces
  a guest channel (1–255, never reused during a room) to the host; kind 2 carries an ordered TCP stream chunk
  of 1–65536 bytes; kind 3 closes a channel. Guests always send channel 0; only
  the service assigns their host-side channel. Clients send text `ping` for an
  automatic `pong`, without touching game messages.
- The native host bridges each announced channel to its loopback Room listener.
  The native guest bridges its loopback join socket to its outbound WebSocket.
  Preserve exact stream bytes and normal Room framing; bound queues, cancel all
  I/O on close, and retain the native handshake before save installation.
- UDP voice is not carried by this stream contract. It must be integrated
  separately before claiming router-free voice chat.

Not release-ready until an actual deployment and an
internet connection between different networks have been completed.

## Separate media endpoint (source only)

A guest's gameplay WebSocket upgrade returns `X-Beetleworx-Media-Token` and
`X-Beetleworx-Media-Channel`. The token is a private 256-bit bearer credential
for that admitted gameplay connection. Do not log it, put it in URLs, or
publish it in presence. It is distinct from the short invitation code.

`GET /v1/rooms/CODE/voice` upgrades another outbound WebSocket. It requires the
same release header plus `Authorization: Bearer ...`: the host uses its host
key, while guests use their media tokens. Existing players can reconnect media
after the native game locks new joins. Replacing or closing media does not
close gameplay. Leaving gameplay closes its media socket and revokes its token.

Media frames are `[2, channel, encrypted datagram...]`, with 1–512 opaque payload
bytes. Guests send channel 0; the service supplies their actual admitted game
channel to the host. Host packets name that channel; the receiving guest sees
channel 0. The native adapter maps host-side game channels to actual native
peer IDs by matching the admitted loopback TCP connection at voice-offer time.
Ciphertext headers do not establish that mapping. Existing native encryption
and replay checks remain. Each media connection has bounded, expiring queues
and independent retries; its retirement never joins socket work on the game
owner. These native paths still require runtime validation.

The media socket has independent framing and rate bounds and cannot send game
control messages. Room generations reject callbacks from expired incarnations.
Close callbacks reply to the close handshake before retirement. Source checks
in `scripts/test-relay-media.ps1` use real local WebSockets, not microphones,
native game worlds, or internet media.
