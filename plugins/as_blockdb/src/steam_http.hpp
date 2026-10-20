// steam_http — minimal asynchronous HTTP client for a Plugify plugin, on the game server's own
// Steam API.
//
// The Metamod module used CSteamGameServerAPIContext + ISteamHTTP from hl2sdk (and, because it
// waited for the response, one detached std::thread per request). s2sdk does not expose HTTP, so
// this helper re-creates the same interface on the server's own libsteam_api.so:
//
//   * the shared object is resolved with dlopen (the s2sdk plugin already links
//     libsteam_api.so, so RTLD_NOLOAD finds it; the game directory is only used as a fallback),
//   * the requests go through the plain C accessors SteamAPI_ISteamHTTP_* exported by that
//     library (no vtable offset assumptions of our own),
//   * completion is delivered through a call result object whose layout mirrors Valve's
//     public CCallbackBase ABI (Run/Run/GetCallbackSizeBytes + flags), exactly what
//     CCallResult<> did inside the Metamod module. HTTPRequestCompleted_t is the documented
//     callback struct; both layouts are guarded with static_asserts below.
//
// Everything runs on the game thread: SendRequest() only submits the request (the Steam API is
// asynchronous), the completion callback records the response and the plugin's next Poll()
// invokes the handler. Requests that never complete (no Steam call-result pump, offline server)
// are released after a timeout, and the handler is then called with transportOk == false.
// Nothing here throws (a plugin must not unwind into the engine), nothing blocks.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace steamhttp {

// EHTTPMethod of steam_api.h.
enum class Method : int32_t {
	Get = 1,
	Head = 2,
	Post = 3,
	Put = 4,
	Delete = 5,
	Options = 6,
	Patch = 7,
};

struct Response {
	bool transportOk = false;  // the request completed (a HTTP status code is available)
	bool successful = false;   // m_bRequestSuccessful of HTTPRequestCompleted_t
	int32_t statusCode = 0;    // HTTP status code (0 when the transport failed)
	std::string body;          // response body (empty on failure)
};

using ResponseHandler = std::function<void(const Response& response)>;

// Resolves libsteam_api.so and its HTTP entry points. Safe to call repeatedly; returns false
// (and logs once per missing symbol group) when HTTP cannot be used on this server.
bool Init();

// True once Init() resolved everything.
bool Available();

// Submits a request; the handler runs on the game thread from Poll() (or with
// transportOk == false when the request timed out / could not be sent). No-op when the
// bindings are unavailable (the handler is not called at all).
void SendRequest(Method method, const std::string& url, const std::string& authorization,
				 const std::string& contentType, const std::string& body, ResponseHandler handler);

// Runs the handlers of finished requests and reaps stuck ones. Call once per frame.
void Poll();

// Releases everything (plugin end).
void Shutdown();

} // namespace steamhttp
