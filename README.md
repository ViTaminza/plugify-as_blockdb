# as_blockdb (Plugify)

BlockDB integration, ported from the Pisex admin_system module 'BlockDB' ([AS] BlockDB by Pisex): reports every ban to api.blockdb.ru (POST/DELETE /v2/bans with the ApiKey), checks the BlockDB ban list when a player connects and announces returning offenders on a Discord webhook, disconnecting players whose distinct-ban count reached AutoBlockBanCount. HTTP runs over the game server's own libsteam_api.so (ISteamHTTP) without blocking the game thread. Config keys (server_name, ApiKey, AutoBlockBanCount, webhook) are unchanged. Pure s2sdk: no pisexcompat, no hudmenu, no local database. Requires the admin_system core.

Plugify port (fork) of **BlockDB ([AS] BlockDB)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=2.18.2), admin_system (>=1.0.0)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `as_blockdb-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_blockdb`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

