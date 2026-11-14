# Beetleworx

Beetleworx aims to be the ultimate upgrade of the MouseTrap engine for Epic Mickey and Epic Mickey 2. It brings the Xbox 360 version of Epic Mickey 2 to Windows through static recompilation and adds support for reading the original Wii game's assets in the newer engine.

## Public testing demo

The demo includes Epic Mickey 2 gameplay and an experimental Epic Mickey compatibility preview. It requires your own game files. The download contains the Windows executable, its dependencies, and patches that are applied to your files during setup.

1. Download the `Beetleworx-PublicDemo` ZIP under Assets on the [release page](https://github.com/Bloticle/beetleworx/releases) and extract it somewhere writable.
2. Open `beetleworx.exe`. Select your Epic Mickey 2 Xbox 360 disc image or extracted game folder when prompted.
3. To try the Epic Mickey preview, also select your Wii disc image (RVZ, ISO, WIA, WBFS, CISO, GCZ or NFS). Both modes need the Epic Mickey 2 files.

The native Options menu includes graphics settings, control rebinding, mouse and cursor controls, and movement tuning. The demo has single-player character switching, sprint, dash, and ground pound. It also includes options for Gremlin dialogue names and Mickey's original ink drips; the ink assets come from the mounted Wii disc. Press backtick and open **Levels** to browse and try levels from either game. Epic Mickey level compatibility varies by area.

In the online demo, open **Online Multiplayer** at the Epic Mickey 2 title menu. The host chooses a campaign and shares an eight-character room code; guests choose **Join copied room code**, then **Ready**. Discord invitations feed the same Join flow. Both players need the same release version and compatible game files. Connection settings are supplied with the download; players do not configure routers. Voice through the relay is unfinished, so use a Discord voice channel while testing.

Use **Support & Updates** to review and save a diagnostic report before opening a [GitHub issue](https://github.com/Bloticle/beetleworx/issues). Nothing is submitted automatically. The menu can also check for and download newer releases. Verified updates install on normal exit, preserving your saves and game files.

## What still needs work

- **Epic Mickey campaign:** The Wii game is not playable from beginning to end yet. Level transitions, progression, quests, encounters, scripts, and audio need work across the campaign. Draw order, material fidelity, NPC animations, collision, and spawning after load or death are still incomplete in some areas. The courtyard is an exploration preview.
- **Online play:** The online demo adds a native lobby, eight-character internet room codes and Discord invitation integration. The lobby allows up to 16 players, but full-size groups and complete campaign playthroughs remain unverified. Public matchmaking, joining a running game, prediction, rollback, reconnect and relayed voice are unfinished. Native actor/HUD handling, transitions, respawns, pants colors and nametags need broader multiplayer validation.
- **High FPS and controls:** Start at 30 FPS. Higher frame limits need more work on animation smoothness, camera response, physics, and input timing. Mouse aiming, switching input devices, and solo Oswald respawns need testing in more areas.
- **Graphics and performance:** HDR and experimental scene lighting are held back from the demo. They need stable transitions and broader GPU testing. Distance-based shadows, upscaling, shader preparation, and high-resolution performance need further validation and tuning.
- **Movement abilities:** The imported Rebrushed clips are for Mickey. Oswald uses his existing animations. Movement, landing damage, and animation transitions need testing across 3D areas, 2D sections, and the two games.
- **Modding:** Runtime texture and model swaps from standard file formats, mods that ship only changed assets, and an in-game entity and asset inspector are planned.
- **Setup and updates:** EM2 setup supports uncompressed Xbox 360 disc images and extracted folders; EM1 accepts RVZ, ISO/GCM, WIA, WBFS, CISO, GCZ and NFS through an embedded reader. Regional asset archives are patched by resource name; matching decoded executable code is still required. Xbox compressed containers, legacy NKit conversions, and more hardware configurations still need coverage. The first demo has no earlier public build to exercise the full live update path.

If an update is interrupted and the game no longer opens, extract a fresh release ZIP. Keep `user_data` for your saves and settings and `game-data` for any disc files imported by setup.

## Credits

EMR movement animations: `@llenarox` on Discord.

## Build from source

Current source includes [cooperative title shutdown](docs/native-guest-shutdown.md), addressing a confirmed hang caused by killing a guest thread while it held a logger lock. Native three-client tests cover Mickey and Oswald recovery and normal process exit. This change is not in the existing demo.6 download.

See [Building Beetleworx](docs/BUILDING.md) for prerequisites and commands. The [development notes](DEVELOPMENT.md) cover the renderer, input, and experimental online code.

Development setup accepts additional Xbox containers and archives, including CSO/ZSO, CCI, standalone CHD, GOD/SVOD, ZArchive, ZIP/ZIP64 and 7z, plus extracted EM1 disc folders. These readers are ahead of the public demo. See [disc format support and validation limits](docs/disc-formats-development.md) for tested cases and remaining work. Matching executable code is still required.

Online remains experimental. Native four-process captures have exercised movement and repeated same-level reloads; the hosted relay has passed a separate connection/data-transfer check. Full gameplay over the relay, real Discord invitations, arbitrary level changes and complete playthroughs remain unverified. A desync or lost player can stop the session.
