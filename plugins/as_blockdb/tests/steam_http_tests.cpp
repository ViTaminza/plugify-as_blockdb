// as_blockdb — tests for the Steam HTTP transport's URL redaction and real error branches.
//
// No network and no libsteam_api.so are used: steam_http.cpp is compiled with
// AS_BLOCKDB_STEAM_HTTP_TESTING and the test injects a fake SteamApi whose CreateHTTPRequest /
// SendHTTPRequest can fail. That exercises the exact logging branches the plugin hits in
// production, including the branch that used to print the Discord webhook URL (which contains its
// token in the path).
//
// Covered:
//   * SafeUrlForLog(): path/query/fragment/userinfo are removed, only scheme://host[:port] is kept,
//     malformed URLs become a fixed placeholder, and no secret can survive into the log line;
//   * CreateHTTPRequest failure: one sanitized log line, no send, no handler call;
//   * SendHTTPRequest failure: sanitized log line, HTTP handle released, no handler call;
//   * empty URL: no request at all;
//   * Shutdown(): a pending request is unregistered/released and its handler is never called
//     (the detached-completion teardown at plugin unload).

#include "steam_http.hpp"
#include "steam_http_testing.hpp"
#include "steam_url.hpp"

#include <plg/plugin.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace plg {
// The only Plugify symbol steam_http.cpp references.
LogFn Log = nullptr;
} // namespace plg

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr, ...)                                                                       \
	do {                                                                                       \
		++g_checks;                                                                            \
		if (!(expr)) {                                                                         \
			++g_failures;                                                                      \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                        \
			std::printf("  (%s)\n", std::string(__VA_ARGS__).c_str());                         \
		}                                                                                      \
	} while (false)

void CheckEq(std::string_view actual, std::string_view expected, const char* what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL %s: got '%s', expected '%s'\n", what, std::string(actual).c_str(),
					std::string(expected).c_str());
	}
}

std::vector<std::string> g_logs;

void FakeLog(std::string_view message, plg::Severity /*severity*/, const plg::source_location& /*location*/) {
	g_logs.emplace_back(message);
}

bool LogContains(std::string_view needle) {
	for (const std::string& entry : g_logs) {
		if (entry.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// --- the fake Steam API -----------------------------------------------------------------------

constexpr int32_t kCreateFailure = 0;
constexpr uint32_t kRequestHandle = 42;
constexpr uint64_t kApiCall = 777;

struct FakeApiState {
	uint32_t createResult = kRequestHandle;
	bool sendResult = true;
	int createCalls = 0;
	int sendCalls = 0;
	int releaseCalls = 0;
	int registerCalls = 0;
	int unregisterCalls = 0;
	std::string lastUrl;
};

FakeApiState g_apiState;

void* FakeGetHttp() {
	return reinterpret_cast<void*>(0x1000);
}

uint32_t FakeCreateHTTPRequest(void* /*http*/, int32_t /*method*/, const char* url) {
	++g_apiState.createCalls;
	g_apiState.lastUrl = url != nullptr ? url : "";
	return g_apiState.createResult;
}

bool FakeSetHTTPRequestHeaderValue(void* /*http*/, uint32_t /*request*/, const char* /*name*/,
								   const char* /*value*/) {
	return true;
}

bool FakeSetHTTPRequestRawPostBody(void* /*http*/, uint32_t /*request*/, const char* /*contentType*/,
								   uint8_t* /*body*/, uint32_t /*size*/) {
	return true;
}

bool FakeSendHTTPRequest(void* /*http*/, uint32_t /*request*/, uint64_t* call) {
	++g_apiState.sendCalls;
	if (g_apiState.sendResult && call != nullptr) {
		*call = kApiCall;
	}
	return g_apiState.sendResult;
}

bool FakeGetHTTPResponseBodySize(void* /*http*/, uint32_t /*request*/, uint32_t* size) {
	if (size != nullptr) {
		*size = 0;
	}
	return true;
}

bool FakeGetHTTPResponseBodyData(void* /*http*/, uint32_t /*request*/, uint8_t* /*buffer*/,
								 uint32_t /*size*/) {
	return true;
}

bool FakeReleaseHTTPRequest(void* /*http*/, uint32_t /*request*/) {
	++g_apiState.releaseCalls;
	return true;
}

void FakeRegisterCallResult(steamhttp::CallbackBase* /*callback*/, uint64_t /*call*/) {
	++g_apiState.registerCalls;
}

void FakeUnregisterCallResult(steamhttp::CallbackBase* /*callback*/, uint64_t /*call*/) {
	++g_apiState.unregisterCalls;
}

steamhttp::SteamApi MakeFakeApi() {
	steamhttp::SteamApi api;
	api.getHttp = &FakeGetHttp;
	api.CreateHTTPRequest = &FakeCreateHTTPRequest;
	api.SetHTTPRequestHeaderValue = &FakeSetHTTPRequestHeaderValue;
	api.SetHTTPRequestRawPostBody = &FakeSetHTTPRequestRawPostBody;
	api.SendHTTPRequest = &FakeSendHTTPRequest;
	api.GetHTTPResponseBodySize = &FakeGetHTTPResponseBodySize;
	api.GetHTTPResponseBodyData = &FakeGetHTTPResponseBodyData;
	api.ReleaseHTTPRequest = &FakeReleaseHTTPRequest;
	api.RegisterCallResult = &FakeRegisterCallResult;
	api.UnregisterCallResult = &FakeUnregisterCallResult;
	return api;
}

void ResetState() {
	steamhttp::Shutdown();
	g_apiState = FakeApiState{};
	g_logs.clear();
	steamhttp::SetApiForTests(MakeFakeApi());
	plg::Log = &FakeLog;
}

constexpr char kWebhookUrl[] = "https://discord.com/api/webhooks/123456789/SECRET-TOKEN?wait=true";
constexpr char kAuthorization[] = "Bearer SECRET-AUTH";

// --- 1. the sanitizer --------------------------------------------------------------------------

void TestSafeUrlForLog() {
	std::printf("-- SafeUrlForLog\n");

	// The Discord webhook token is in the path - only the origin may survive.
	const std::string webhook = steamhttp::SafeUrlForLog(kWebhookUrl);
	CheckEq(webhook, "https://discord.com", "webhook origin only");
	CHECK(webhook.find("SECRET") == std::string::npos, "the webhook token is not logged");
	CHECK(webhook.find("123456789") == std::string::npos, "the webhook id/path is not logged");
	CHECK(webhook.find("wait=true") == std::string::npos, "the query is not logged");

	CheckEq(steamhttp::SafeUrlForLog("https://api.blockdb.ru/v2/bans/76561198000000003"),
			"https://api.blockdb.ru", "steamid path is removed");
	CheckEq(steamhttp::SafeUrlForLog("https://user:pass@host.example:8443/x?q=1#frag"),
			"https://host.example:8443", "userinfo/query/fragment are removed");
	CheckEq(steamhttp::SafeUrlForLog("https://[::1]:8080/x"), "https://[::1]:8080",
			"IPv6 literal with a port");
	CheckEq(steamhttp::SafeUrlForLog("HTTPS://Host.Example/Path"), "HTTPS://Host.Example",
			"the scheme case is preserved");
	CheckEq(steamhttp::SafeUrlForLog("http://127.0.0.1:27015"), "http://127.0.0.1:27015",
			"a plain origin stays unchanged");

	// Malformed input must not leak anything either.
	for (const char* invalid : {"", "not a url", "https://", "https:///path", "https://host:bad/x",
								"https://ho st/x", "://host/x", "https://host:/x"}) {
		CheckEq(steamhttp::SafeUrlForLog(invalid), "<redacted-url>", invalid);
	}
}

// --- 2. the real steam_http error branches ----------------------------------------------------

void TestCreateRequestFailure() {
	std::printf("-- CreateHTTPRequest failure\n");

	ResetState();
	g_apiState.createResult = kCreateFailure;

	bool handled = false;
	steamhttp::SendRequest(steamhttp::Method::Post, kWebhookUrl, kAuthorization, "application/json",
						   "{\"content\":null}", [&handled](const steamhttp::Response&) { handled = true; });

	CHECK(!handled, "the handler is not called when CreateHTTPRequest fails");
	CHECK(g_apiState.createCalls == 1, "one CreateHTTPRequest attempt");
	CHECK(g_apiState.sendCalls == 0, "nothing is sent");
	CHECK(LogContains("CreateHTTPRequest failed for https://discord.com"), "sanitized URL is logged");
	CHECK(!LogContains("SECRET-TOKEN") && !LogContains("SECRET-AUTH") && !LogContains("/api/webhooks"),
		  "no token, auth or path in the log");
}

void TestSendRequestFailure() {
	std::printf("-- SendHTTPRequest failure\n");

	ResetState();
	g_apiState.sendResult = false;

	bool handled = false;
	steamhttp::SendRequest(steamhttp::Method::Post, kWebhookUrl, kAuthorization, "application/json",
						   "{\"content\":null}", [&handled](const steamhttp::Response&) { handled = true; });

	CHECK(!handled, "the handler is not called when SendHTTPRequest fails");
	CHECK(g_apiState.createCalls == 1, "the request was created");
	CHECK(g_apiState.sendCalls == 1, "one send attempt");
	CHECK(g_apiState.releaseCalls == 1, "the HTTP handle is released");
	CHECK(LogContains("SendHTTPRequest failed for https://discord.com"), "sanitized URL is logged");
	CHECK(!LogContains("SECRET-TOKEN") && !LogContains("SECRET-AUTH") && !LogContains("/api/webhooks"),
		  "no token, auth or path in the log");
}

void TestEmptyUrl() {
	std::printf("-- empty URL\n");

	ResetState();
	bool handled = false;
	steamhttp::SendRequest(steamhttp::Method::Get, "", "", "application/json", "",
						   [&handled](const steamhttp::Response&) { handled = true; });
	CHECK(!handled);
	CHECK(g_apiState.createCalls == 0, "an empty URL never reaches the Steam API");
	CHECK(g_logs.empty(), "an empty URL logs nothing");
}

void TestShutdownDropsPending() {
	std::printf("-- Shutdown drops a pending handler\n");

	ResetState();
	bool handled = false;
	steamhttp::SendRequest(steamhttp::Method::Get, "https://api.blockdb.ru/v2/bans/765",
						   "Bearer SECRET-AUTH", "application/json", "",
						   [&handled](const steamhttp::Response&) { handled = true; });
	CHECK(g_apiState.registerCalls == 1, "a successful send registers a call result");
	CHECK(g_apiState.sendCalls == 1);

	steamhttp::Shutdown();
	CHECK(!handled, "Shutdown never runs the pending handler");
	CHECK(g_apiState.unregisterCalls == 1, "the call result is unregistered");
	CHECK(g_apiState.releaseCalls == 1, "the HTTP handle is released");
	CHECK(!LogContains("SECRET-AUTH"), "the auth header never appears in the logs");
}

} // namespace

int main() {
	std::printf("steam_http_tests (fake Steam API, real steam_http.cpp error branches)\n");

	steamhttp::SetApiForTests(MakeFakeApi());
	plg::Log = &FakeLog;

	TestSafeUrlForLog();
	TestCreateRequestFailure();
	TestSendRequestFailure();
	TestEmptyUrl();
	TestShutdownDropsPending();

	steamhttp::Shutdown();
	steamhttp::ResetApiForTests();

	std::printf("steam_http_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
