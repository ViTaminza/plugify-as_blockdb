#include "steam_http.hpp"

#include <pisex/output.hpp>

#include <s2sdk/s2sdk.hpp> // GetGameDirectory for the libsteam_api fallback path

#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace steamhttp {
namespace {

constexpr char kLogTag[] = "steam_http";
// Requests that neither complete nor fail are released after this long (the original waited
// forever in its detached thread).
constexpr auto kRequestTimeout = std::chrono::seconds(45);

// --- the Steam ABI pieces we touch ------------------------------------------------------------
//
// HTTPRequestCompleted_t (isteamhttp.h): the struct the callback receives. Layout verified with
// the static_asserts - 4 bytes handle, padding, 8 bytes context value, bool, padding, status
// code, body size.
struct HTTPRequestCompleted_t {
	enum { k_iCallback = 2102 }; // k_iSteamHTTPCallbacks (2101) + 1

	uint32_t m_hRequest;
	uint64_t m_ulContextValue;
	bool m_bRequestSuccessful;
	int32_t m_eStatusCode;
	uint32_t m_unBodySize;
};
static_assert(sizeof(HTTPRequestCompleted_t) == 32, "HTTPRequestCompleted_t layout");
static_assert(offsetof(HTTPRequestCompleted_t, m_ulContextValue) == 8, "HTTPRequestCompleted_t layout");
static_assert(offsetof(HTTPRequestCompleted_t, m_bRequestSuccessful) == 16, "HTTPRequestCompleted_t layout");
static_assert(offsetof(HTTPRequestCompleted_t, m_eStatusCode) == 20, "HTTPRequestCompleted_t layout");
static_assert(offsetof(HTTPRequestCompleted_t, m_unBodySize) == 24, "HTTPRequestCompleted_t layout");

// Valve's CCallbackBase (steam_api_common.h). Deliberately no virtual destructor: the vtable of
// the object handed to SteamAPI_RegisterCallResult must start with Run(void*),
// Run(void*, bool, SteamAPICall_t) and GetCallbackSizeBytes(), and the two data members must sit
// right after the vptr - that is the binary interface the library was compiled against.
class CallbackBase {
public:
	virtual void Run(void* pvParam) = 0;
	virtual void Run(void* pvParam, bool bIOFailure, uint64_t hSteamAPICall) = 0;
	virtual int GetCallbackSizeBytes() = 0;

	enum {
		k_ECallbackFlagsRegistered = 0x01,
		k_ECallbackFlagsGameServer = 0x02,
	};
	uint8_t m_nCallbackFlags;
	int32_t m_iCallback;

protected:
	CallbackBase() : m_nCallbackFlags(0), m_iCallback(0) {}
	~CallbackBase() = default; // never deleted through this type
};
// The C ABI boundary: vptr, then the flags byte and the callback id - the library reads and
// writes them at these offsets. (offsetof on a polymorphic type is what GCC would warn about;
// a size check pins the same layout without it.)
struct CallbackBaseLayoutProbe {
	uint8_t flags;
	int32_t callback;
};
static_assert(sizeof(CallbackBase) == sizeof(void*) + sizeof(CallbackBaseLayoutProbe),
			  "CallbackBase must be vptr + flags + callback id");

// --- resolved entry points --------------------------------------------------------------------
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

SteamApi g_api;
bool g_loadFailureLogged = false;
bool g_symbolFailure = false; // permanent: a missing symbol cannot appear later

void LogError(std::string_view message) {
	pisex::ErrorLog("[%s] %.*s", kLogTag, static_cast<int>(message.size()), message.data());
}

template <typename Fn>
Fn Resolve(void* module, const char* name) {
	return reinterpret_cast<Fn>(dlsym(module, name));
}

void* OpenSteamApi() {
	// The s2sdk plugin declares libsteam_api.so as a dependency, so it is already resident in
	// the server process; RTLD_NOLOAD returns that handle without loading anything.
	if (void* handle = dlopen("libsteam_api.so", RTLD_NOW | RTLD_NOLOAD)) {
		return handle;
	}
	// Fallbacks: the usual soname spelling and paths relative to the game directory.
	std::vector<std::string> candidates = {"libsteam_api.so"};
	if (__s2sdk_GetGameDirectory) {
		const std::string gameDir(s2sdk::GetGameDirectory());
		if (!gameDir.empty()) {
			candidates.push_back(gameDir + "/bin/linuxsteamrt64/libsteam_api.so");
			candidates.push_back(gameDir + "/../bin/linuxsteamrt64/libsteam_api.so");
			candidates.push_back(gameDir + "/../linuxsteamrt64/libsteam_api.so");
		}
	}
	candidates.push_back("bin/linuxsteamrt64/libsteam_api.so");
	candidates.push_back("../bin/linuxsteamrt64/libsteam_api.so");
	for (const std::string& candidate : candidates) {
		if (void* handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL)) {
			return handle;
		}
	}
	return nullptr;
}

bool ResolveApi() {
	if (g_symbolFailure) {
		return false;
	}
	if (g_api.getHttp == nullptr) {
		g_api.module = OpenSteamApi();
		if (g_api.module == nullptr) {
			if (!g_loadFailureLogged) {
				g_loadFailureLogged = true; // once: SendRequest() probes again on every request
				LogError("libsteam_api.so could not be loaded - the BlockDB API and the Discord webhook stay disabled");
			}
			return false;
		}
		// The exported accessor of this server build; try newest first.
		for (const char* name : {"SteamAPI_SteamGameServerHTTP_v004", "SteamAPI_SteamGameServerHTTP_v003",
								 "SteamAPI_SteamGameServerHTTP_v002"}) {
			g_api.getHttp = Resolve<decltype(g_api.getHttp)>(g_api.module, name);
			if (g_api.getHttp != nullptr) {
				break;
			}
		}
		g_api.CreateHTTPRequest = Resolve<decltype(g_api.CreateHTTPRequest)>(g_api.module, "SteamAPI_ISteamHTTP_CreateHTTPRequest");
		g_api.SetHTTPRequestHeaderValue =
			Resolve<decltype(g_api.SetHTTPRequestHeaderValue)>(g_api.module, "SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue");
		g_api.SetHTTPRequestRawPostBody =
			Resolve<decltype(g_api.SetHTTPRequestRawPostBody)>(g_api.module, "SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody");
		g_api.SendHTTPRequest = Resolve<decltype(g_api.SendHTTPRequest)>(g_api.module, "SteamAPI_ISteamHTTP_SendHTTPRequest");
		g_api.GetHTTPResponseBodySize =
			Resolve<decltype(g_api.GetHTTPResponseBodySize)>(g_api.module, "SteamAPI_ISteamHTTP_GetHTTPResponseBodySize");
		g_api.GetHTTPResponseBodyData =
			Resolve<decltype(g_api.GetHTTPResponseBodyData)>(g_api.module, "SteamAPI_ISteamHTTP_GetHTTPResponseBodyData");
		g_api.ReleaseHTTPRequest = Resolve<decltype(g_api.ReleaseHTTPRequest)>(g_api.module, "SteamAPI_ISteamHTTP_ReleaseHTTPRequest");
		g_api.RegisterCallResult = Resolve<decltype(g_api.RegisterCallResult)>(g_api.module, "SteamAPI_RegisterCallResult");
		g_api.UnregisterCallResult = Resolve<decltype(g_api.UnregisterCallResult)>(g_api.module, "SteamAPI_UnregisterCallResult");

		const bool complete = g_api.getHttp && g_api.CreateHTTPRequest && g_api.SetHTTPRequestHeaderValue &&
							  g_api.SetHTTPRequestRawPostBody && g_api.SendHTTPRequest && g_api.ReleaseHTTPRequest &&
							  g_api.RegisterCallResult && g_api.UnregisterCallResult;
		if (!complete) {
			g_symbolFailure = true;
			LogError("libsteam_api.so does not export the expected ISteamHTTP entry points - HTTP stays disabled");
			g_api.getHttp = nullptr;
			return false;
		}
	}

	if (g_api.http == nullptr) {
		// The interface only exists once the engine initialized the game server context; probing
		// again on every request is cheap and covers a plugin that loaded early.
		g_api.http = g_api.getHttp();
	}
	return g_api.http != nullptr;
}

// --- pending requests ---------------------------------------------------------------------
struct Pending;

class RequestCallback final : public CallbackBase {
public:
	explicit RequestCallback(Pending* pending) : m_pending(pending) {
		m_nCallbackFlags = static_cast<uint8_t>(k_ECallbackFlagsGameServer);
		m_iCallback = HTTPRequestCompleted_t::k_iCallback;
	}

	void Run(void* pvParam) override;
	void Run(void* pvParam, bool bIOFailure, uint64_t hSteamAPICall) override;
	int GetCallbackSizeBytes() override { return static_cast<int>(sizeof(HTTPRequestCompleted_t)); }

	Pending* m_pending; // valid as long as the Pending exists (unique_ptr, never moved)
};

struct Pending {
	Pending() : callback(this) {}

	RequestCallback callback;
	uint32_t request = 0; // HTTPRequestHandle
	uint64_t call = 0;    // SteamAPICall_t
	bool ready = false;
	Response response;
	ResponseHandler handler;
	std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
};

std::vector<std::unique_ptr<Pending>> g_pending;

void RequestCallback::Run(void* pvParam, bool bIOFailure, uint64_t /*hSteamAPICall*/) {
	try {
		Pending& pending = *m_pending;
		pending.ready = true; // the handler runs from Poll() on the game thread
		if (bIOFailure || pvParam == nullptr) {
			pending.response.transportOk = false;
			return;
		}
		const auto* completed = static_cast<const HTTPRequestCompleted_t*>(pvParam);
		Response& response = pending.response;
		response.transportOk = true;
		response.successful = completed->m_bRequestSuccessful;
		response.statusCode = completed->m_eStatusCode;
		// Never trust the announced body size with an unbounded allocation.
		constexpr uint32_t kMaxBodySize = 4u * 1024u * 1024u;
		if (completed->m_bRequestSuccessful && completed->m_unBodySize > 0) {
			if (completed->m_unBodySize > kMaxBodySize) {
				response.body.clear();
				LogError("HTTP response body exceeds the 4 MiB limit and was dropped");
				return;
			}
			std::string body(static_cast<std::size_t>(completed->m_unBodySize), '\0');
			uint32_t size = completed->m_unBodySize;
			if (g_api.GetHTTPResponseBodyData && g_api.http &&
				g_api.GetHTTPResponseBodyData(g_api.http, completed->m_hRequest, reinterpret_cast<uint8_t*>(body.data()), size)) {
				response.body = std::move(body);
			}
		}
	} catch (...) {
		// Never unwind into the Steam callback pump; the request is reaped by the timeout sweep.
	}
}

void RequestCallback::Run(void* pvParam) {
	Run(pvParam, false, 0);
}

// Releases the HTTP handle of a request that is being dropped.
void Release(Pending& pending) {
	if (pending.request != 0 && g_api.ReleaseHTTPRequest && g_api.http) {
		g_api.ReleaseHTTPRequest(g_api.http, pending.request);
		pending.request = 0;
	}
}

} // namespace

bool Init() {
	return ResolveApi();
}

bool Available() {
	return g_api.http != nullptr;
}

void SendRequest(Method method, const std::string& url, const std::string& authorization,
				 const std::string& contentType, const std::string& body, ResponseHandler handler) {
	if (!ResolveApi()) {
		return; // logged once by ResolveApi; the handler is not called
	}
	if (url.empty()) {
		return;
	}

	auto owned = std::make_unique<Pending>();
	owned->handler = std::move(handler);
	Pending& pending = *owned;

	const uint32_t request =
		g_api.CreateHTTPRequest(g_api.http, static_cast<int32_t>(method), url.c_str());
	if (request == 0) {
		LogError("CreateHTTPRequest failed for " + url);
		return;
	}
	pending.request = request;

	if (!authorization.empty()) {
		g_api.SetHTTPRequestHeaderValue(g_api.http, request, "Authorization", authorization.c_str());
	}
	const std::string content = contentType.empty() ? std::string("application/json") : contentType;
	g_api.SetHTTPRequestHeaderValue(g_api.http, request, "Content-Type", content.c_str());
	if (!body.empty()) {
		g_api.SetHTTPRequestRawPostBody(g_api.http, request, content.c_str(),
										reinterpret_cast<uint8_t*>(const_cast<char*>(body.data())),
										static_cast<uint32_t>(body.size()));
	}

	uint64_t call = 0;
	if (!g_api.SendHTTPRequest(g_api.http, request, &call)) {
		LogError("SendHTTPRequest failed for " + url);
		g_api.ReleaseHTTPRequest(g_api.http, request);
		return;
	}
	pending.call = call;
	pending.startedAt = std::chrono::steady_clock::now();
	g_pending.push_back(std::move(owned)); // the address of pending never changes again
	g_api.RegisterCallResult(&pending.callback, call);
}

void Poll() {
	if (g_pending.empty()) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	std::vector<std::pair<Response, ResponseHandler>> finished;

	for (auto it = g_pending.begin(); it != g_pending.end();) {
		Pending& pending = **it;
		if (pending.ready) {
			finished.emplace_back(std::move(pending.response), std::move(pending.handler));
			Release(pending);
			it = g_pending.erase(it);
			continue;
		}
		if (now - pending.startedAt > kRequestTimeout) {
			// No completion (no Steam call-result pump, or a hung server): drop the call result
			// and release the handle so neither can leak.
			if (g_api.UnregisterCallResult && pending.call != 0) {
				g_api.UnregisterCallResult(&pending.callback, pending.call);
			}
			finished.emplace_back(Response{}, std::move(pending.handler)); // transportOk == false
			Release(pending);
			it = g_pending.erase(it);
			continue;
		}
		++it;
	}

	// Run the handlers last: they may start new requests and touch the plugin's state.
	for (auto& [response, handler] : finished) {
		try {
			if (handler) {
				handler(response);
			}
		} catch (...) {
			// a handler must not take the server down either
		}
	}
}

void Shutdown() {
	Poll(); // deliver what already arrived
	for (auto it = g_pending.begin(); it != g_pending.end();) {
		Pending& pending = **it;
		if (g_api.UnregisterCallResult && pending.call != 0) {
			g_api.UnregisterCallResult(&pending.callback, pending.call);
		}
		Release(pending);
		it = g_pending.erase(it);
	}
}

} // namespace steamhttp
