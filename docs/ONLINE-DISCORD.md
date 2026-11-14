# Discord invitations

The native integration uses Beetleworx Application ID `1262682777153962059`.
It communicates with the Discord desktop client through its local named-pipe
[RPC interface](https://github.com/discord/discord-api-docs/blob/main/developers/topics/rpc.mdx).
It does not require a bot token, client secret, helper executable or additional
Discord DLL. The application ID is a public identifier.

## Implemented flow

- A connected internet lobby publishes its actual player count and capacity.
  Its join secret contains the eight-character relay room code.
- Players send invitations through Discord's game invitation interface.
  Beetleworx subscribes to accepted `ACTIVITY_JOIN` events and passes validated
  room codes into the normal native Join flow. It does not send messages or
  accept arbitrary requests on the player's behalf.
- At the title menu, an accepted invite opens the native online page and starts
  asynchronous room preparation. Guests still choose Ready in the lobby.
- Invites received during gameplay remain queued until returning to the title.
  They do not unload the player's active campaign automatically.
- The game registers the application's `discord-1262682777153962059` launch
  protocol for the current Windows user. Its command starts the canonical
  `beetleworx.exe`, bypasses the game chooser, and waits at the Epic Mickey 2
  title menu for Discord to deliver the invite. No untrusted URL text is
  interpolated into the command line. No administrator access is required.
- Join secrets are removed when gameplay starts. This build does not yet
  support joining a native Room that has already started.
- Cancelling a lobby clears that room's activity. Per-room ownership prevents
  cleanup of an old room from clearing a newer room's presence.

The local IPC worker uses bounded frames, overlapped I/O, cancellation, a
handshake deadline, activity acknowledgement deadlines and reconnect backoff.
Activity changes are throttled. It does not inspect chats, friend lists or
private Discord account information. Isolated private QA profiles do not
connect to Discord or modify launch registration.

`em2_discord_invites=false` disables the integration. The public Application ID
can be configured with `em2_discord_application_id` if a future distribution
uses a different application.

## Current limits

Discord invitations provide discovery and launch; the hosted relay still carries
the game connection. The relay is deployed; online demo packages supply its
endpoint automatically through online-service.json. This integration compiles
and its native menu has been rebuilt, but an actual invitation between two
Discord accounts and full gameplay over the relay remain unverified.

The full Social SDK friends UI, in-game Discord invite picker, joining a running
game, public matchmaking and relayed voice remain unfinished. Existing native
campaign synchronization and HUD limitations also remain.
