// as_blockdb — the Steam HTTP entry points the transport resolves at runtime.
//
// This header exists so the transport can be exercised in tests with an injected fake API (see
// steam_http_testing.hpp). Production resolves the same signatures with dlsym() from the server's
// own libsteam_api.so and never sets them by hand.

#pragma once

#include <cstdint>

namespace steamhttp {

// Valve's CCallbackBase, as seen by SteamAPI_RegisterCallResult. Defined in steam_http.cpp; the
// fake API only needs the pointer type.
class CallbackBase;

struct SteamApi {
	void* module = nullptr;
	void* (*getHttp)() = nullptr; // SteamAPI_SteamGameServerHTTP_vNNN
	uint32_t (*CreateHTTPRequest)(void*, int32_t, const char*) = nullptr;
	bool (*SetHTTPRequestHeaderValue)(void*, uint32_t, const char*, const char*) = nullptr;
	bool (*SetHTTPRequestRawPostBody)(void*, uint32_t, const char*, uint8_t*, uint32_t) = nullptr;
	bool (*SendHTTPRequest)(void*, uint32_t, uint64_t*) = nullptr;
	bool (*GetHTTPResponseBodySize)(void*, uint32_t, uint32_t*) = nullptr;
	bool (*GetHTTPResponseBodyData)(void*, uint32_t, uint8_t*, uint32_t) = nullptr;
	bool (*ReleaseHTTPRequest)(void*, uint32_t) = nullptr;
	void (*RegisterCallResult)(CallbackBase*, uint64_t) = nullptr;
	void (*UnregisterCallResult)(CallbackBase*, uint64_t) = nullptr;
	void* http = nullptr; // ISteamHTTP*
};

} // namespace steamhttp
