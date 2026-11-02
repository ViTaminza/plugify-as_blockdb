// as_blockdb — test-only injection point for the Steam HTTP transport.
//
// Built only into the steam_http_tests executable (the source is compiled with
// AS_BLOCKDB_STEAM_HTTP_TESTING). The plugin target never sees these functions; production
// always resolves the real entry points from libsteam_api.so.

#pragma once

#include "steam_http_api.hpp"

namespace steamhttp {

// Replaces the resolved Steam API with a fake and makes ResolveApi() trust it. `api.http` may be
// null; it is filled from api.getHttp() when that is set.
void SetApiForTests(const SteamApi& api);

// Drops the injected API and returns the transport to normal (dlsym) resolution.
void ResetApiForTests();

} // namespace steamhttp
