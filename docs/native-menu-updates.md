# Native menus and startup updates

These changes are included in demo.7. Validation scope and remaining checks are listed below.

## Menus and input

Settings use a category grid, with separate setting names and value controls. Status text is not selectable. Controls open with explicit player and device choices. Keyboard/mouse, Xbox, PlayStation and Nintendo menu prompts can be selected manually; Automatic switches between keyboard/mouse and controller prompts.

The mouse bridge converts physical window coordinates into the Flash coordinate space independently of 3D render resolution. It requires a press and release inside the same control, ignores hidden and inactive pages, and does not let a parked cursor replace controller selection. Native sliders and Back/confirm controls use the same bridge. Controller binding capture polls independently of gameplay and reports failed disk writes in the menu.

Online begins with Host Game and Join Game. Join accepts a typed or pasted eight-character code. The lobby shows player names, character silhouettes, name/pants colors and ready marks alongside text chat. Hosts can remove a guest before starting. Confirmation binds the target to the roster revision, so a departure/rejoin cannot cause the host to remove a replacement player accidentally. The host cannot remove themselves, and a frozen gameplay roster cannot be changed through this action.

## Startup updates

Official installations check GitHub's releases list on launch, including demo prereleases. Checks do not download a game build. A newer version is offered through the existing Flash `DialogBox.summon` notification, with Update and Not now actions.

Update downloads and verifies the selected release. It restarts the canonical executable after normal shutdown, using the existing installer and rollback path. Keep playing leaves the update scheduled for normal exit instead. Not now suppresses the same offer for that launch; the next launch checks again. Offline checks and development builds do not block playing. Save files and imported game data remain outside the release payload.

## Verification and remaining checks

- The shared Flash mouse method bodies pass activation, hover ownership, visibility, press/release, slider and navigation tests. Native coordinate and prompt tests cover output scaling, snapshot consistency, focus loss and manual/automatic choices.
- Controller capture and immediate persistence passed without gameplay polling. Failure/retry tests cover a blocked binding file path.
- The Room tests exercise sixteen connected sockets, ordered authoritative chat, host-only kick, recipient notification, replacement admission and frozen-roster rejection. Graceful TCP close now parses buffered control messages before retiring the connection.
- Two isolated native windows loaded the new lobby panels, entered gameplay and exchanged text chat. Both exited normally; regular saves, settings and crash files were unchanged. Evidence: `analysis/native-lobby-20261114-183507-429cae51/result.json`. This capture predates the startup-update notification addition.
- Update fixtures exercise launch checks with both legacy preference values, no download before acceptance, dismissal, reoffering on a later launch, verified staging, offline failure and existing installation/rollback checks. Flash notification method tests cover both choices, progress, one verified restart request, Keep playing, errors and existing-dialog ownership.

Physical mouse/controller acceptance across all retail screens and a live newer-release Flash notification/restart still need end-to-end verification. The reported EM2 campaign-save problem has not been reproduced or claimed fixed. Full sixteen-player native gameplay and a complete online campaign remain unverified.
