# Beetleworx

Beetleworx aims to be the ultimate upgrade of the MouseTrap engine for Epic Mickey and Epic Mickey 2. It brings the Xbox 360 version of Epic Mickey 2 to Windows through static recompilation and adds support for reading the original Wii game's assets in the newer engine.

## Public testing demo

The demo includes Epic Mickey 2 gameplay and an experimental Epic Mickey compatibility preview. It requires your own game files. The download contains the Windows executable, its dependencies, and patches that are applied to your files during setup.

1. Download the `Beetleworx-PublicDemo` ZIP under Assets on the [release page](https://github.com/Bloticle/beetleworx/releases) and extract it somewhere writable.
2. Open `beetleworx.exe`. Select your Epic Mickey 2 Xbox 360 disc image or extracted game folder when prompted.
3. To try the Epic Mickey preview, also select your Wii disc image (RVZ, ISO, WIA, WBFS, CISO, GCZ or NFS). Both modes need the Epic Mickey 2 files.

The native Options menu includes graphics settings, control rebinding, mouse and cursor controls, and movement tuning. The demo has single-player character switching, sprint, dash, and ground pound. It also includes options for Gremlin dialogue names and Mickey's original ink drips; the ink assets come from the mounted Wii disc. Press backtick and open **Levels** to browse and try levels from either game. Epic Mickey level compatibility varies by area.

In the online demo, open **Online Multiplayer** at the Epic Mickey 2 title menu. The host selects **Host Game** and shares an eight-character room code; guests choose **Join Game**, type or paste the code, then select **Ready**. Discord invitations feed the same Join flow. Both players need the same release version and compatible game files. Connection settings are supplied with the download; players do not configure routers. Voice through the relay is unfinished, so use a Discord voice channel while testing.

Use **Support & Updates** to review and save a diagnostic report before opening a [GitHub issue](https://github.com/Bloticle/beetleworx/issues). Nothing is submitted automatically. The menu can also check for and download newer releases. Verified updates install on normal exit, preserving your saves and game files.

## Demo.9 online menu replacement

Online Multiplayer now opens an ImGui desktop window. The native main-menu controls hide while it is open. Hosting, room-code entry, Discord invites, the sixteen-player roster, name/color controls, host kick confirmation, text chat and voice settings share this window. Networking and the original save-to-game handoff use the existing backend. Mouse activation and connection/lobby/error layouts have focused headless checks; a complete multiplayer playthrough remains unverified. Controller navigation is supported; desktop text fields currently require a keyboard.

## Demo.8 menu hotfix

Demo.8 fixes missing text glyphs, brings settings content inside the center panel, and repositions whole native EM2 online buttons so their artwork and clickable areas move together. It removes the added online button cards, makes lobby panel backgrounds opaque, and avoids restarting hover animations for every mouse movement. Held keyboard keys are blocked at the transition into the pause menu, preventing Pause from immediately becoming Back. Flash compilation and focused input checks cover these changes; live visual confirmation is still pending.

## Demo.7 changes

The source adds mouse support for native menu controls, a category grid for settings, separate input-device selection, and automatic or manual keyboard/mouse, Xbox, PlayStation and Nintendo menu prompts. Online starts with Host Game and Join Game, including typed room codes. The lobby shows the player roster and text chat, with a host-only kick action.

Official builds now check for updates at launch and ask through the game's existing Flash notification dialog before downloading. Update verifies the download and restarts Beetleworx; Keep playing schedules installation for normal exit. Not now skips the offer until the next launch. These changes are included in demo.7. See [menu and update validation](docs/native-menu-updates.md) for coverage and remaining checks.
## What still needs work

- **Epic Mickey campaign:** The Wii game is not playable from beginning to end yet. Level transitions, progression, quests, encounters, scripts, and audio need work across the campaign. Draw order, material fidelity, NPC animations, collision, and spawning after load or death are still incomplete in some areas. The courtyard is an exploration preview.
- **Online play:** The online demo adds a native lobby, eight-character internet room codes and Discord invitation integration. The lobby allows up to 16 players, but full-size groups and complete campaign playthroughs remain unverified. Public matchmaking, joining a running game, prediction, rollback, reconnect and relayed voice are unfinished. Native actor/HUD handling, transitions, respawns, pants colors and nametags need broader multiplayer validation.
- **High FPS and controls:** Start at 30 FPS. Higher frame limits need more work on animation smoothness, camera response, physics, and input timing. Mouse aiming, switching input devices, and solo Oswald respawns need testing in more areas.
- **Graphics and performance:** HDR and experimental scene lighting are held back from the demo. They need stable transitions and broader GPU testing. Distance-based shadows, upscaling, shader preparation, and high-resolution performance need further validation and tuning.
- **Movement abilities:** The imported Rebrushed clips are for Mickey. Oswald uses his existing animations. Movement, landing damage, and animation transitions need testing across 3D areas, 2D sections, and the two games.
- **Modding:** Runtime texture and model swaps from standard file formats, mods that ship only changed assets, and an in-game entity and asset inspector are planned.
- **Setup and updates:** EM2 setup supports uncompressed Xbox 360 disc images and extracted folders; EM1 accepts RVZ, ISO/GCM, WIA, WBFS, CISO, GCZ and NFS through an embedded reader. Regional asset archives are patched by resource name; matching decoded executable code is still required. Xbox compressed containers, legacy NKit conversions, and more hardware configurations still need coverage. The new startup notification and live update restart still need end-to-end coverage with a newer official release.

If an update is interrupted and the game no longer opens, extract a fresh release ZIP. Keep `user_data` for your saves and settings and `game-data` for any disc files imported by setup.

## Credits

EMR movement animations: `@llenarox` on Discord.

## Build from source

Current source includes [cooperative title shutdown](docs/native-guest-shutdown.md), addressing a confirmed hang caused by killing a guest thread while it held a logger lock. Native three-client tests cover Mickey and Oswald recovery and normal process exit. This change is not in the existing demo.6 download.

Current source also uses [mapped precompiled shader packs](docs/mapped-shader-packs.md). Alternating startup tests saved about 178 MiB of private memory while retaining the same preloaded shaders and pipelines. Shader data and cache fallback checks pass; the complete three-client level route still reaches the test machine's memory guard. No gameplay FPS gain is claimed from this change.

See [Building Beetleworx](docs/BUILDING.md) for prerequisites and commands. The [development notes](DEVELOPMENT.md) cover the renderer, input, and experimental online code.

Development setup accepts additional Xbox containers and archives, including CSO/ZSO, CCI, standalone CHD, GOD/SVOD, ZArchive, ZIP/ZIP64 and 7z, plus extracted EM1 disc folders. These readers are ahead of the public demo. See [disc format support and validation limits](docs/disc-formats-development.md) for tested cases and remaining work. Matching executable code is still required.

Online remains experimental. Native four-process captures have exercised movement and repeated same-level reloads; the hosted relay has passed a separate connection/data-transfer check. Two native processes on one PC now have [hosted-relay gameplay, chat and a level-transition check](docs/online-relay-gameplay.md). Separate-machine play, real Discord invitations, arbitrary level changes and complete playthroughs remain unverified. Native cutscene input restriction and return now have [three-client coverage for both characters](docs/online-cutscene-input-epochs.md). Interactive cutscene choices and complete story scenes still need work. [Experimental skip routing has transport and native-handler guard tests](docs/online-cutscene-skip.md); real authored-scene skipping and credits remain unverified or unfinished. [Original direct-movie playback and held skipping](docs/online-direct-movie-status.md) now have three-client natural-exit, configured three-second remote skip, and restored-control coverage. Original `StartSequence` movie playback now has [three-client entry, synchronized exit, and restored-control coverage](docs/online-movie-sequence.md). [Movie Accept skipping](docs/online-movie-skip.md) now routes any human's press through the native delay/unskippable guards, with three-client delayed-skip and held-input return tests. A [carried-button fix](docs/online-cutscene-held-return.md) prevents held input from becoming an unsolicited Jump when gameplay bindings resume. An [authored Autotopia conversation test](docs/online-authored-conversation.md) completes all 21 original events across three native clients; cinematic skipping remains unverified. A desync or lost player can stop the session.
