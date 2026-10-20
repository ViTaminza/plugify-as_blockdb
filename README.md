# as_blockdb (Plugify)

BlockDB integration, ported from the Pisex admin_system module 'BlockDB' ([AS] BlockDB by Pisex): reports every ban to api.blockdb.ru (POST/DELETE /v2/bans with the ApiKey), checks the BlockDB ban list when a player connects and announces returning offenders on a Discord webhook, disconnecting players whose distinct-ban count reached AutoBlockBanCount. HTTP runs over the game server's own libsteam_api.so (ISteamHTTP) without blocking the game thread. Config keys (server_name, ApiKey, AutoBlockBanCount, webhook) are unchanged. Requires the admin_system core.

Plugify port (fork) of **BlockDB ([AS] BlockDB)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, admin_system, hudmenu (optional)

## Installation
Download `as_blockdb-1.0.1.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_blockdb`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

