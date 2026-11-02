// as_blockdb tests — no engine and no network: the plugin TU is compiled in (its handlers live in
// an anonymous namespace), every __s2sdk_* / __admin_system_* binding slot is a recording fake, and
// the Steam HTTP transport is replaced by a recording fake linked in place of src/steam_http.cpp.
//
// Covered:
//   * src/config.hpp — the self-contained KV1 reader, the shipped BlockDB.ini files (module and
//     live, with their comments), the getter semantics of the original and the single unchanged
//     production path <configs>/admin_system/BlockDB.ini;
//   * PITFALLS B1/B2 — the map gate: not one s2sdk client call before the first map, between
//     OnMapEnd and the next OnMapStart, or when the plugin was loaded into the map-change window;
//   * PITFALLS C2 — slot range, connected/SourceTV checks, the controller check of the name,
//     "<unknown>" -> "", negative slots are never indexed;
//   * PITFALLS C12 — the "player_disconnect" Pre hook, GetEventPlayerSlot("userid"), a negative
//     resolved slot, Continue on every path, unhook on unload;
//   * the BlockDB API half (the "DB" of this plugin; it has no SQL database): ban POST, unban
//     DELETE, ban query GET, Authorization/Bearer headers, request order, transport/HTTP/JSON
//     errors, the kick threshold, the slot-reuse guard and the Discord webhook payload;
//   * the 60 s dedupe + the once-per-second reconcile (seeding, new players, bots);
//   * lifecycle: start/end/reload, symmetric removal of every listener/hook/callback, no console
//     command or command listener registered (the module has no command surface / permissions),
//     zero-binding degradation and detached callbacks after OnPluginEnd.
//
// Run by ./test.sh (build/tests/as_blockdb_tests). No pisexcompat, no plgdb, no database.

#include "../src/plugin.cpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

// --- the fake Steam HTTP transport -------------------------------------------------------------
// src/steam_http.cpp is not linked into the test; these definitions satisfy the header and record
// every request. Deliver() invokes a completion handler synchronously, like Poll() would.

namespace steamhttp {

struct FakeRequest {
	Method method = Method::Get;
	std::string url;
	std::string authorization;
	std::string contentType;
	std::string body;
};

std::vector<FakeRequest> g_requests;
std::map<std::size_t, ResponseHandler> g_pending;
bool g_initResult = true;
bool g_available = true;
int g_initCalls = 0;
int g_pollCalls = 0;
int g_shutdownCalls = 0;

bool Init() {
	++g_initCalls;
	return g_initResult;
}

bool Available() {
	return g_available;
}

void SendRequest(Method method, const std::string& url, const std::string& authorization,
				 const std::string& contentType, const std::string& body, ResponseHandler handler) {
	g_requests.push_back(FakeRequest{method, url, authorization, contentType, body});
	if (handler) {
		g_pending[g_requests.size() - 1] = std::move(handler);
	}
}

void Poll() {
	++g_pollCalls;
}

void Shutdown() {
	++g_shutdownCalls;
	g_pending.clear();
}

std::size_t PendingCount() {
	return g_pending.size();
}

ResponseHandler PendingHandler(std::size_t index) {
	const auto it = g_pending.find(index);
	return it == g_pending.end() ? ResponseHandler{} : it->second;
}

// Completes the request `index`; false when it has no handler or was already dropped.
bool Deliver(std::size_t index, const Response& response) {
	const auto it = g_pending.find(index);
	if (it == g_pending.end()) {
		return false;
	}
	ResponseHandler handler = std::move(it->second);
	g_pending.erase(it);
	if (handler) {
		handler(response);
	}
	return true;
}

void Reset() {
	g_requests.clear();
	g_pending.clear();
	g_initResult = true;
	g_available = true;
	g_initCalls = 0;
	g_pollCalls = 0;
	g_shutdownCalls = 0;
}

std::vector<std::size_t> FindRequests(Method method, std::string_view urlPart) {
	std::vector<std::size_t> out;
	for (std::size_t i = 0; i < g_requests.size(); ++i) {
		const FakeRequest& request = g_requests[i];
		if (request.method == method && request.url.find(urlPart) != std::string::npos) {
			out.push_back(i);
		}
	}
	return out;
}

std::size_t LastRequest() {
	return g_requests.empty() ? static_cast<std::size_t>(-1) : g_requests.size() - 1;
}

} // namespace steamhttp

namespace {

int g_checks = 0;
int g_failures = 0;

inline void CheckMessage() {}

inline void CheckMessage(std::string_view message) {
	std::printf("  (%s)\n", std::string(message).c_str());
}

#define CHECK(expr, ...)                                                                       \
	do {                                                                                       \
		++g_checks;                                                                            \
		if (!(expr)) {                                                                         \
			++g_failures;                                                                      \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                        \
			CheckMessage(__VA_ARGS__);                                                         \
		}                                                                                      \
	} while (false)

#define CHECK_NOTHROW(expr)                                                                    \
	do {                                                                                       \
		++g_checks;                                                                            \
		try {                                                                                  \
			(expr);                                                                            \
		} catch (...) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL %s:%d: unexpected exception: %s\n", __FILE__, __LINE__, #expr);   \
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

void CheckEqInt(int64_t actual, int64_t expected, const char* what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL %s: got %lld, expected %lld\n", what, static_cast<long long>(actual),
					static_cast<long long>(expected));
	}
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

// --- the fake plugify host --------------------------------------------------------------------

std::string g_configsDir;
std::string g_dataDir;

plg::string FakeGetConfigsDir() {
	return plg::string(g_configsDir.c_str());
}

plg::string FakeGetDataDir() {
	return plg::string(g_dataDir.c_str());
}

struct FakeLogEntry {
	plg::Severity severity = plg::Severity::Info;
	std::string message;
};

std::vector<FakeLogEntry> g_logs;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& /*location*/) {
	g_logs.push_back(FakeLogEntry{severity, std::string(message)});
}

bool Logged(plg::Severity severity, std::string_view needle) {
	for (const FakeLogEntry& entry : g_logs) {
		if (entry.severity == severity && entry.message.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool AnyLogContains(std::string_view needle) {
	for (const FakeLogEntry& entry : g_logs) {
		if (entry.message.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// --- the fake map lifecycle -------------------------------------------------------------------

s2sdk::OnMapStartCallback g_onMapStart = nullptr;
s2sdk::OnMapEndCallback g_onMapEnd = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_onMapStart = callback;
	++g_mapStartRegisters;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
	++g_mapStartUnregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_onMapEnd = callback;
	++g_mapEndRegisters;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
	++g_mapEndUnregisters;
}

void MapStart() {
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void MapEnd() {
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

// --- the fake client authorization listener ---------------------------------------------------

s2sdk::OnClientAuthenticatedCallback g_authCallback = nullptr;
int g_authRegisters = 0;
int g_authUnregisters = 0;

void FakeAuthRegister(s2sdk::OnClientAuthenticatedCallback callback) {
	g_authCallback = callback;
	++g_authRegisters;
}

void FakeAuthUnregister(s2sdk::OnClientAuthenticatedCallback callback) {
	if (g_authCallback == callback) {
		g_authCallback = nullptr;
	}
	++g_authUnregisters;
}

void FireAuth(int32_t slot, uint64_t steamid64) {
	if (g_authCallback != nullptr) {
		g_authCallback(slot, steamid64);
	}
}

// --- the fake client layer --------------------------------------------------------------------

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool hasController = false;
	bool fake = false;
	bool authorized = false;
	std::string name;
	uint64_t steamid64 = 0;
};

std::vector<FakeClient> g_clients(64);

int g_clientCalls = 0; // every fake below that is a client query counts into this
int g_connectedCalls = 0;
int g_inGameCalls = 0;
int g_sourceTvCalls = 0;
int g_controllerCalls = 0;
int g_nameCalls = 0;
int g_steamIdCalls = 0;
int g_fakeCalls = 0;
int g_authorizedCalls = 0;

const FakeClient& Slot(int32_t slot) {
	static const FakeClient empty;
	return slot >= 0 && slot < static_cast<int32_t>(g_clients.size())
			   ? g_clients[static_cast<std::size_t>(slot)]
			   : empty;
}

void ResetClientCounters() {
	g_clientCalls = 0;
	g_connectedCalls = 0;
	g_inGameCalls = 0;
	g_sourceTvCalls = 0;
	g_controllerCalls = 0;
	g_nameCalls = 0;
	g_steamIdCalls = 0;
	g_fakeCalls = 0;
	g_authorizedCalls = 0;
}

bool FakeIsClientConnected(int32_t slot) {
	++g_clientCalls;
	++g_connectedCalls;
	return Slot(slot).connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_clientCalls;
	++g_inGameCalls;
	return Slot(slot).inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_clientCalls;
	++g_sourceTvCalls;
	return Slot(slot).sourceTV;
}

bool FakeIsFakeClient(int32_t slot) {
	++g_clientCalls;
	++g_fakeCalls;
	return Slot(slot).fake;
}

bool FakeIsClientAuthorized(int32_t slot) {
	++g_clientCalls;
	++g_authorizedCalls;
	return Slot(slot).authorized;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_clientCalls;
	++g_controllerCalls;
	if (!Slot(slot).hasController) {
		return nullptr;
	}
	// Never dereferenced by the plugin: it only asks whether a controller exists.
	return reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000) + static_cast<uintptr_t>(slot));
}

plg::string FakeGetClientName(int32_t slot) {
	++g_clientCalls;
	++g_nameCalls;
	// s2sdk answers "<unknown>" when it cannot resolve the client name.
	return plg::string(Slot(slot).name.empty() ? "<unknown>" : Slot(slot).name.c_str());
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_clientCalls;
	++g_steamIdCalls;
	return Slot(slot).steamid64;
}

// --- the fake kick ----------------------------------------------------------------------------

struct FakeKick {
	int32_t slot = -1;
	s2sdk::NetworkDisconnectionReason reason = s2sdk::NetworkDisconnectionReason::Kickbanadded;
};

std::vector<FakeKick> g_kicks;

void FakeKickClient(int32_t slot, s2sdk::NetworkDisconnectionReason reason, const plg::string& /*message*/) {
	g_kicks.push_back(FakeKick{slot, reason});
}

// --- the fake player_disconnect event ---------------------------------------------------------

s2sdk::EventCallback g_disconnectHook = nullptr;
std::vector<std::string> g_hookedEvents;
std::vector<s2sdk::HookMode> g_hookedModes;
std::vector<std::string> g_unhookedEvents;
std::vector<s2sdk::HookMode> g_unhookedModes;
s2sdk::EventHookError g_hookEventResult = s2sdk::EventHookError::Okay;
int g_hookEventCalls = 0;
int g_unhookEventCalls = 0;

int g_fakeEventStorage = 0;
int32_t g_eventSlot = -1;
std::string g_eventKey;
int g_eventSlotCalls = 0;

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback,
									s2sdk::HookMode mode) {
	++g_hookEventCalls;
	g_hookedEvents.push_back(ToStd(name));
	g_hookedModes.push_back(mode);
	if (g_hookEventResult != s2sdk::EventHookError::Okay) {
		return g_hookEventResult;
	}
	if (ToStd(name) == "player_disconnect" && mode == s2sdk::HookMode::Pre) {
		g_disconnectHook = callback;
	}
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback callback,
									  s2sdk::HookMode mode) {
	++g_unhookEventCalls;
	g_unhookedEvents.push_back(ToStd(name));
	g_unhookedModes.push_back(mode);
	if (g_disconnectHook == callback) {
		g_disconnectHook = nullptr;
	}
	(void)mode;
	return s2sdk::EventHookError::Okay;
}

int32_t FakeGetEventPlayerSlot(void* event, const plg::string& key) {
	++g_eventSlotCalls;
	g_eventKey = ToStd(key);
	if (event != &g_fakeEventStorage) {
		return -1;
	}
	return g_eventSlot;
}

s2sdk::ResultType FireDisconnect(int32_t slot) {
	g_eventSlot = slot;
	if (g_disconnectHook == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	return g_disconnectHook(plg::string("player_disconnect"), &g_fakeEventStorage, false);
}

// --- the fake admin_system bindings -----------------------------------------------------------

admin_system::PlayerPunishCallback g_punishCallback = nullptr;
admin_system::OfflinePlayerUnpunishCallback g_unpunishCallback = nullptr;
int g_punishRegisters = 0;
int g_punishUnregisters = 0;
int g_unpunishRegisters = 0;
int g_unpunishUnregisters = 0;

float FakeGetPluginVersion() {
	return 1.0f;
}

void FakePunishRegister(admin_system::PlayerPunishCallback callback) {
	++g_punishRegisters;
	g_punishCallback = callback;
}

void FakePunishUnregister(admin_system::PlayerPunishCallback callback) {
	++g_punishUnregisters;
	if (g_punishCallback == callback) {
		g_punishCallback = nullptr;
	}
}

void FakeUnpunishRegister(admin_system::OfflinePlayerUnpunishCallback callback) {
	++g_unpunishRegisters;
	g_unpunishCallback = callback;
}

void FakeUnpunishUnregister(admin_system::OfflinePlayerUnpunishCallback callback) {
	++g_unpunishUnregisters;
	if (g_unpunishCallback == callback) {
		g_unpunishCallback = nullptr;
	}
}

// Through the registered trampoline, i.e. the path the admin core uses.
void FirePunish(int32_t slot, int32_t type, int32_t time, const char* reason) {
	if (g_punishCallback != nullptr) {
		g_punishCallback(slot, type, time, plg::string(reason != nullptr ? reason : ""), -1);
	}
}

void FireUnpunish(const char* steamid64, int32_t type) {
	if (g_unpunishCallback != nullptr) {
		g_unpunishCallback(plg::string(steamid64 != nullptr ? steamid64 : ""), type, -1);
	}
}

// --- the fake command surface (the module must never touch it) --------------------------------

int g_addConsoleCommandCalls = 0;
int g_addCommandListenerCalls = 0;

bool FakeAddConsoleCommand(const plg::string& /*name*/, const plg::string& /*description*/,
						   s2sdk::ConVarFlag /*flags*/, s2sdk::ConCommandCallback /*callback*/,
						   s2sdk::HookMode /*mode*/) {
	++g_addConsoleCommandCalls;
	return true;
}

bool FakeAddCommandListener(const plg::string& /*name*/, s2sdk::ConCommandCallback /*callback*/,
							s2sdk::HookMode /*mode*/) {
	++g_addCommandListenerCalls;
	return true;
}

// --- installing the fakes ---------------------------------------------------------------------

void InstallFakes() {
	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;

	__s2sdk_OnClientAuthenticated_Register = &FakeAuthRegister;
	__s2sdk_OnClientAuthenticated_Unregister = &FakeAuthUnregister;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_IsClientAuthorized = &FakeIsClientAuthorized;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_KickClient = &FakeKickClient;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;

	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;

	__admin_system_GetPluginVersion = &FakeGetPluginVersion;
	__admin_system_OnPlayerPunish_Register = &FakePunishRegister;
	__admin_system_OnPlayerPunish_Unregister = &FakePunishUnregister;
	__admin_system_OnOfflinePlayerUnpunish_Register = &FakeUnpunishRegister;
	__admin_system_OnOfflinePlayerUnpunish_Unregister = &FakeUnpunishUnregister;

	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::GetDataDir = &FakeGetDataDir;
	plg::Log = &FakeLog;
}

// --- the throw-away workspace -----------------------------------------------------------------

std::string g_baseDir;

std::string RawConfigPath() {
	return g_baseDir + "/addons/configs/admin_system/BlockDB.ini";
}

std::string ConfigPath() {
	return g_baseDir + "/configs/admin_system/BlockDB.ini";
}

std::string TranslationsConfigPath() {
	return g_baseDir + "/configs/admin_system/translations/BlockDB.ini";
}

std::string DataConfigPath() {
	return g_baseDir + "/data/admin_system/BlockDB.ini";
}

void RemoveFile(const std::string& path) {
	std::error_code error;
	std::filesystem::remove(path, error);
}

void WriteFile(const std::string& path, std::string_view text) {
	std::filesystem::create_directories(std::filesystem::path(path).parent_path());
	std::FILE* file = std::fopen(path.c_str(), "wb");
	if (file == nullptr) {
		std::printf("FAIL: could not write %s\n", path.c_str());
		++g_failures;
		return;
	}
	std::fwrite(text.data(), 1, text.size(), file);
	std::fclose(file);
}

void WriteConfig(std::string_view text) {
	WriteFile(ConfigPath(), text);
}

void RemoveConfig() {
	RemoveFile(RawConfigPath());
	RemoveFile(ConfigPath());
	RemoveFile(TranslationsConfigPath());
	RemoveFile(DataConfigPath());
}

// Runs `fn` with `g_baseDir` as the server working directory, so the raw relative candidate
// "addons/configs/admin_system/BlockDB.ini" can be exercised. Restores the old cwd.
bool WithBaseCwd(const std::function<void()>& fn) {
	std::error_code error;
	const std::filesystem::path oldCwd = std::filesystem::current_path(error);
	if (error) {
		return false;
	}
	std::filesystem::current_path(g_baseDir, error);
	if (error) {
		return false;
	}
	fn();
	std::filesystem::current_path(oldCwd, error);
	return !error;
}

// A config that enables every feature the tests need; the hosts are reserved .invalid names, so
// even a mistakenly real transport could never reach anything.
constexpr std::string_view kTestConfig = R"("BlockDB"
{
    "server_name"           "Test Server"
    "ApiKey"                "test-api-key"
    "AutoBlockBanCount"     "3"
    "webhook"               "https://discord.invalid/webhook"
})";

// --- the fake world reset ---------------------------------------------------------------------

void ResetWorld() {
	g_clients.assign(64, FakeClient{});
	ResetClientCounters();
	g_kicks.clear();

	g_onMapStart = nullptr;
	g_onMapEnd = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;

	g_authCallback = nullptr;
	g_authRegisters = 0;
	g_authUnregisters = 0;

	g_disconnectHook = nullptr;
	g_hookedEvents.clear();
	g_hookedModes.clear();
	g_unhookedEvents.clear();
	g_unhookedModes.clear();
	g_hookEventResult = s2sdk::EventHookError::Okay;
	g_hookEventCalls = 0;
	g_unhookEventCalls = 0;
	g_eventSlot = -1;
	g_eventKey.clear();
	g_eventSlotCalls = 0;

	g_punishCallback = nullptr;
	g_unpunishCallback = nullptr;
	g_punishRegisters = 0;
	g_punishUnregisters = 0;
	g_unpunishRegisters = 0;
	g_unpunishUnregisters = 0;

	g_addConsoleCommandCalls = 0;
	g_addCommandListenerCalls = 0;

	g_logs.clear();
	RemoveConfig();
	steamhttp::Reset();

	g_configsDir = g_baseDir + "/configs";
	g_dataDir = g_baseDir + "/data";
	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::GetDataDir = &FakeGetDataDir;
	plg::Log = &FakeLog;

	// The plugin globals a fresh OnPluginStart would reset anyway; explicit here so a test that
	// calls a handler without starting cannot observe the previous test's state.
	g_config = as_blockdb::Config{};
	g_seenSteamIds.fill(0);
	g_pendingKicks.fill(PendingKick{});
	g_lastConnectTime.clear();
	g_seenSeeded = false;
	g_sinceReconcile = std::chrono::milliseconds(0);
	g_mapReady = false;
	g_mapEndedSinceStart = false;
	g_firstUpdateDone = false;
	g_mapListeners = false;
	g_authListener = false;
	g_disconnectHooked = false;
	g_active = false;
	g_pAdminApi = nullptr;
}

void PrepareTest() {
	// Stops whatever a previous test left behind (OnPluginEnd is safe when nothing is registered)
	// and starts the next test from an empty fake world.
	g_asBlockDBPlugin.OnPluginEnd();
	ResetWorld();
}

void FinishTest() {
	g_asBlockDBPlugin.OnPluginEnd();
	ResetWorld();
}

// --- request helpers --------------------------------------------------------------------------

steamhttp::FakeRequest RequestAt(std::size_t index) {
	if (index >= steamhttp::g_requests.size()) {
		return {};
	}
	return steamhttp::g_requests[index];
}

std::string JsonField(const std::string& body, const char* key) {
	const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
	if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains(key)) {
		return {};
	}
	if (parsed[key].is_string()) {
		return parsed[key].get<std::string>();
	}
	if (parsed[key].is_number()) {
		return std::to_string(parsed[key].get<int64_t>());
	}
	return {};
}

// --- config ------------------------------------------------------------------------------------

void TestKv1Reader() {
	std::printf("-- KV1 reader\n");

	const std::string_view text = R"(
// The Pisex file, with the comments an admin leaves behind.
"BlockDB"
{
    "server_name"           "MIRAGE #1" // Название сервера
    /* the rest is unquoted on purpose */
    ApiKey                  "key"
    "AutoBlockBanCount"     "3"
    webhook                 "https://example.invalid/hook"
})";

	as_blockdb::Config config;
	CHECK(as_blockdb::LoadConfigFromText(text, config));
	CheckEq(config.serverName, "MIRAGE #1", "server_name");
	CheckEq(config.apiKey, "key", "ApiKey");
	CheckEqInt(config.autoBlockBanCount, 3, "AutoBlockBanCount");
	CheckEq(config.webhook, "https://example.invalid/hook", "webhook");

	// A BOM, escapes and a case-insensitive lookup of the keys.
	const std::string bom = std::string("\xEF\xBB\xBF") +
							R"("BlockDB" { "SERVER_NAME" "a\"b\\c" "ApiKey" "k" })";
	config = as_blockdb::Config{};
	CHECK(as_blockdb::LoadConfigFromText(bom, config));
	CheckEq(config.serverName, "a\"b\\c", "server_name after BOM/escapes");

	// #base / #include / #define lines are skipped, not followed.
	const std::string_view directives = R"(#base "admin_system/base.ini"
#include "shared.ini"
#define BLOCKDB
"BlockDB" { "server_name" "five" })";
	config = as_blockdb::Config{};
	CHECK(as_blockdb::LoadConfigFromText(directives, config));
	CheckEq(config.serverName, "five", "server_name after preprocessor lines");

	// Top-level keys (no "BlockDB" block) are read directly.
	config = as_blockdb::Config{};
	CHECK(as_blockdb::LoadConfigFromText(R"("server_name" "nine" "AutoBlockBanCount" "9")", config));
	CheckEq(config.serverName, "nine", "top-level server_name");
	CheckEqInt(config.autoBlockBanCount, 9, "top-level AutoBlockBanCount");

	// Two top-level blocks: nothing is merged, so every key stays at its default.
	config = as_blockdb::Config{};
	CHECK(as_blockdb::LoadConfigFromText(
		R"("BlockDB" { "server_name" "four" } "Other" { "server_name" "six" })", config));
	CheckEq(config.serverName, "", "two blocks keep the default");
}

void TestKv1Getters() {
	std::printf("-- KV1 getter semantics\n");

	const auto root = *as_blockdb::ParseKv1(
		R"("BlockDB" { "a" "12" "b" "1.5" "c" "abc" "d" "-3" "e" "" })");
	CheckEqInt(root.GetInt("a", 99), 12, "GetInt plain");
	CheckEqInt(root.GetInt("b", 99), 1, "GetInt falls back to atoi");
	CheckEqInt(root.GetInt("c", 99), 0, "GetInt non-numeric");
	CheckEqInt(root.GetInt("d", 99), -3, "GetInt negative");
	CheckEqInt(root.GetInt("e", 99), 99, "GetInt empty value keeps the default");
	CheckEqInt(root.GetInt("missing", 99), 99, "GetInt missing key");
	CheckEqInt(root.GetInt("A", 99), 12, "GetInt case-insensitive key");

	const auto bools = *as_blockdb::ParseKv1(
		R"("BlockDB" {
			"t" "true" "y" "YES" "o" "on"
			"f" "false" "n" "No" "x" "off"
			"two" "2" "zero" "0" "junk" "abc" "empty" ""
		})");
	CHECK(bools.GetBool("t", false));
	CHECK(bools.GetBool("y", false));
	CHECK(bools.GetBool("o", false));
	CHECK(!bools.GetBool("f", true));
	CHECK(!bools.GetBool("n", true));
	CHECK(!bools.GetBool("x", true));
	CHECK(bools.GetBool("two", false));
	CHECK(!bools.GetBool("zero", true));
	CHECK(!bools.GetBool("junk", true));
	CHECK(bools.GetBool("empty", true), "an empty value keeps the default");
	CHECK(!bools.GetBool("empty", false));
	CHECK(bools.GetBool("missing", true), "a missing key keeps the default");

	CheckEq(bools.GetString("missing", "def"), "def", "GetString default");
	CheckEq(bools.GetString("t"), "true", "GetString value");
	const auto blockOnly = *as_blockdb::ParseKv1(R"("BlockDB" { "sub" { "k" "v" } })");
	CheckEq(blockOnly.GetString("sub", "def"), "def", "a block child has no value");
}

void TestKv1Malformed() {
	std::printf("-- KV1 reader: malformed files are rejected whole\n");

	as_blockdb::Config config;
	config.serverName = "keep";
	config.apiKey = "keep-key";
	config.autoBlockBanCount = 42;
	config.webhook = "keep-hook";

	CHECK(!as_blockdb::LoadConfigFromText(R"("BlockDB" { "server_name" "1")", config)); // no '}'
	CHECK(!as_blockdb::LoadConfigFromText(R"("BlockDB" { "server_name" "1 })", config)); // open quote
	CHECK(!as_blockdb::LoadConfigFromText(R"("BlockDB" { "server_name" "1" } })", config)); // stray '}'
	CheckEq(config.serverName, "keep", "config untouched by a rejected file");
	CheckEq(config.apiKey, "keep-key", "apiKey untouched");
	CheckEqInt(config.autoBlockBanCount, 42, "count untouched");
	CheckEq(config.webhook, "keep-hook", "webhook untouched");

	// Empty input parses as an empty root (no keys), i.e. it is *not* malformed - the defaults
	// stay, exactly like an empty KeyValues file.
	as_blockdb::Config empty;
	CHECK(as_blockdb::LoadConfigFromText("", empty));
	CheckEq(empty.serverName, "", "empty file keeps the default");
	CheckEqInt(empty.autoBlockBanCount, 0, "empty file keeps the count default");
}

void TestConfigCandidates() {
	std::printf("-- config candidates (pisex ResolvePath order)\n");

	const std::vector<std::string> candidates = as_blockdb::ConfigCandidates("/srv/configs", "/srv/data");
	CHECK(candidates.size() == 4);
	CheckEq(candidates[0], "addons/configs/admin_system/BlockDB.ini", "raw cwd path is first");
	CheckEq(candidates[1], "/srv/configs/admin_system/BlockDB.ini", "configs/admin_system second");
	CheckEq(candidates[2], "/srv/configs/admin_system/translations/BlockDB.ini",
			"configs/admin_system/translations third");
	CheckEq(candidates[3], "/srv/data/admin_system/BlockDB.ini", "data/admin_system fourth");

	const std::vector<std::string> trailing = as_blockdb::ConfigCandidates("/srv/configs/", "/srv/data/");
	CHECK(trailing.size() == 4);
	CheckEq(trailing[1], "/srv/configs/admin_system/BlockDB.ini", "trailing slash is normalised");
	CheckEq(trailing[3], "/srv/data/admin_system/BlockDB.ini", "trailing data slash is normalised");

	// pisexcompat with empty roots still produced the relative "admin_system/..." candidates.
	const std::vector<std::string> empty = as_blockdb::ConfigCandidates("", "");
	CHECK(empty.size() == 4);
	CheckEq(empty[0], "addons/configs/admin_system/BlockDB.ini", "raw path without roots");
	CheckEq(empty[1], "admin_system/BlockDB.ini", "relative config dir");
	CheckEq(empty[2], "admin_system/translations/BlockDB.ini", "relative translations dir");
	CheckEq(empty[3], "admin_system/BlockDB.ini", "relative data dir");

	CheckEq(as_blockdb::JoinPath("", "BlockDB.ini"), "BlockDB.ini", "JoinPath empty base");
	CheckEq(as_blockdb::JoinPath("/a/b", ""), "/a/b", "JoinPath empty name");
	CheckEq(as_blockdb::JoinPath("/a/b", "BlockDB.ini"), "/a/b/BlockDB.ini", "JoinPath plain");
	CheckEq(as_blockdb::JoinPath("/a/b/", "BlockDB.ini"), "/a/b/BlockDB.ini", "JoinPath trailing slash");
	CheckEq(as_blockdb::JoinPath("/a/b", "/BlockDB.ini"), "/a/b/BlockDB.ini", "JoinPath leading slash");
}

void TestShippedConfigs() {
	std::printf("-- shipped BlockDB.ini files\n");

	const std::string dataDir = AS_BLOCKDB_TEST_DATA_DIR;

	// The module repository file and the live production file (they are identical today): a
	// "BlockDB" block, trailing Russian // comments, AutoBlockBanCount 3.
	for (const char* name : {"/blockdb_module.ini", "/blockdb_live.ini"}) {
		const std::string path = dataDir + name;
		CHECK(as_blockdb::FileExists(path));
		as_blockdb::Config config;
		CHECK(as_blockdb::LoadConfigFromFile(path, config));
		CheckEq(config.serverName, "MIRAGE #1", "shipped server_name");
		CheckEq(config.apiKey, "", "shipped ApiKey");
		CheckEqInt(config.autoBlockBanCount, 3, "shipped AutoBlockBanCount");
		CheckEq(config.webhook, "", "shipped webhook");
	}

	// The disabled variant used by the tests below.
	const std::string disabled = dataDir + "/blockdb_disabled.ini";
	CHECK(as_blockdb::FileExists(disabled));
	as_blockdb::Config config;
	CHECK(as_blockdb::LoadConfigFromFile(disabled, config));
	CheckEq(config.serverName, "TEST #2", "disabled server_name");
	CheckEq(config.apiKey, "test-api-key", "disabled ApiKey");
	CheckEqInt(config.autoBlockBanCount, 0, "disabled AutoBlockBanCount");
	CheckEq(config.webhook, "https://discord.invalid/hook", "disabled webhook");

	// A missing file is not an error for the caller: false and the values stay untouched.
	as_blockdb::Config untouched;
	untouched.serverName = "eleven";
	CHECK(!as_blockdb::LoadConfigFromFile("/nonexistent/admin_system/BlockDB.ini", untouched));
	CheckEq(untouched.serverName, "eleven", "config untouched by a missing file");
}

void TestLoadConfigWalk() {
	std::printf("-- LoadConfig() first-existing authority and fallbacks\n");

	// 1. The configs/admin_system file is loaded when the raw cwd file is absent. A secret key
	// and webhook must never be printed by the loader.
	PrepareTest();
	const std::string secretKey = "SECRET-API-KEY-123";
	const std::string secretHook = "https://discord.com/api/webhooks/1/SECRET-HOOK-TOKEN";
	WriteConfig(R"("BlockDB" {
		"server_name" "Test Server"
		"ApiKey" ")" + secretKey + R"("
		"AutoBlockBanCount" "3"
		"webhook" ")" + secretHook + R"("
	})");
	CHECK(WithBaseCwd([] { LoadConfig(); }));
	CheckEq(g_config.serverName, "Test Server", "server_name");
	CheckEq(g_config.apiKey, secretKey, "ApiKey");
	CheckEqInt(g_config.autoBlockBanCount, 3, "AutoBlockBanCount");
	CheckEq(g_config.webhook, secretHook, "webhook");
	CHECK(Logged(plg::Severity::Info, ConfigPath()));
	CHECK(!AnyLogContains(secretKey), "the API key is never logged");
	CHECK(!AnyLogContains(secretHook), "the webhook URL is never logged");

	// 2. The raw cwd-relative file has priority over the configs one.
	PrepareTest();
	WriteFile(RawConfigPath(), R"("BlockDB" { "server_name" "raw" "AutoBlockBanCount" "7" })");
	WriteConfig(R"("BlockDB" { "server_name" "decoy" "AutoBlockBanCount" "9" })");
	CHECK(WithBaseCwd([] { LoadConfig(); }));
	CheckEq(g_config.serverName, "raw", "the raw file wins");
	CheckEqInt(g_config.autoBlockBanCount, 7, "the raw values win");
	CHECK(Logged(plg::Severity::Info, as_blockdb::kLegacyRawPath));
	CHECK(!Logged(plg::Severity::Info, ConfigPath()), "the decoy file is not loaded");

	// 3. A malformed raw file is authoritative: no silent fall-through to the decoy.
	PrepareTest();
	WriteFile(RawConfigPath(), R"("BlockDB" { "server_name" "broken")");
	WriteConfig(R"("BlockDB" { "server_name" "decoy" "AutoBlockBanCount" "9" })");
	CHECK(WithBaseCwd([] { LoadConfig(); }));
	CheckEq(g_config.serverName, "", "a malformed raw file keeps the defaults");
	CheckEqInt(g_config.autoBlockBanCount, 0, "no decoy values after a malformed raw file");
	CHECK(Logged(plg::Severity::Error, "Failed to load " + std::string(as_blockdb::kLegacyRawPath)));
	CHECK(!Logged(plg::Severity::Info, ConfigPath()), "the decoy after a malformed raw is not read");

	// 4. The translations and then data fallbacks are used when the earlier candidates are absent.
	PrepareTest();
	WriteFile(TranslationsConfigPath(), R"("BlockDB" { "server_name" "translations" })");
	CHECK(WithBaseCwd([] { LoadConfig(); }));
	CheckEq(g_config.serverName, "translations", "translations fallback");
	CHECK(Logged(plg::Severity::Info, TranslationsConfigPath()));

	PrepareTest();
	WriteFile(DataConfigPath(), R"("BlockDB" { "server_name" "data" })");
	CHECK(WithBaseCwd([] { LoadConfig(); }));
	CheckEq(g_config.serverName, "data", "data fallback");
	CHECK(Logged(plg::Severity::Info, DataConfigPath()));

	// 5. Nothing exists: defaults plus one error naming every candidate, in the pisex order.
	PrepareTest();
	CHECK(WithBaseCwd([] { LoadConfig(); }));
	CheckEq(g_config.serverName, "", "default server_name");
	CheckEq(g_config.apiKey, "", "default ApiKey");
	CheckEqInt(g_config.autoBlockBanCount, 0, "default AutoBlockBanCount");
	CHECK(Logged(plg::Severity::Error, "BlockDB.ini not found (looked at addons/configs/admin_system/BlockDB.ini, " +
										   ConfigPath() + ", " + TranslationsConfigPath() + ", " + DataConfigPath() + ")"));

	// 6. Missing configs/data callbacks still degrade to the raw/relative candidates.
	PrepareTest();
	plg::GetConfigsDir = nullptr;
	plg::GetDataDir = nullptr;
	CHECK(WithBaseCwd([] { LoadConfig(); }));
	CheckEq(g_config.apiKey, "", "defaults without the host dirs");
	CHECK(Logged(plg::Severity::Error, "admin_system/BlockDB.ini"));

	// 7. The live production file through the normal configs candidate.
	PrepareTest();
	std::filesystem::copy_file(std::string(AS_BLOCKDB_TEST_DATA_DIR) + "/blockdb_live.ini", ConfigPath(),
							   std::filesystem::copy_options::overwrite_existing);
	CHECK(WithBaseCwd([] { LoadConfig(); }));
	CheckEq(g_config.serverName, "MIRAGE #1", "live server_name");
	CheckEqInt(g_config.autoBlockBanCount, 3, "live AutoBlockBanCount");

	FinishTest();
}

// --- lifecycle and the map gate ---------------------------------------------------------------

void TestStartRegistrations() {
	std::printf("-- OnPluginStart registrations\n");

	PrepareTest();
	WriteConfig(kTestConfig);

	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart returns Ok (never Failed)");
	CHECK(g_active);
	CHECK(!g_mapReady, "the gate is shut on plugin start");
	CheckEqInt(steamhttp::g_initCalls, 1, "steamhttp::Init is called once");

	// PITFALLS B1: the map listeners.
	CheckEqInt(g_mapStartRegisters, 1, "the map start listener is registered");
	CheckEqInt(g_mapEndRegisters, 1, "the map end listener is registered");
	CHECK(g_mapListeners);
	CHECK(g_onMapStart == &OnMapStartListener);
	CHECK(g_onMapEnd == &OnMapEndListener);

	// The authorization listener and the player_disconnect Pre hook (PITFALLS C12).
	CheckEqInt(g_authRegisters, 1, "OnClientAuthenticated is registered");
	CHECK(g_authListener);
	CHECK(g_authCallback == &OnClientAuthenticatedListener);
	CheckEqInt(g_hookEventCalls, 1, "one event hook");
	CHECK(g_hookedEvents.size() == 1);
	CheckEq(g_hookedEvents[0], "player_disconnect", "hooked event name");
	CHECK(g_hookedModes.size() == 1 && g_hookedModes[0] == s2sdk::HookMode::Pre, "the event hook is Pre");
	CHECK(g_disconnectHook == &OnPlayerDisconnectEvent);
	CHECK(g_disconnectHooked);

	// The admin core callbacks.
	CheckEqInt(g_punishRegisters, 1, "OnPlayerPunish is registered");
	CheckEqInt(g_unpunishRegisters, 1, "OnOfflinePlayerUnpunish is registered");

	// C11: the module has no command surface at all.
	CheckEqInt(g_addConsoleCommandCalls, 0, "no AddConsoleCommand");
	CheckEqInt(g_addCommandListenerCalls, 0, "no AddCommandListener");

	CheckEq(g_config.serverName, "Test Server", "the config was loaded");
	CHECK(Logged(plg::Severity::Info, "ready (server_name Test Server, AutoBlockBanCount 3)"));

	FinishTest();
}

void TestMapGate() {
	std::printf("-- the map gate\n");

	PrepareTest();
	WriteConfig(kTestConfig);
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);

	g_clients[3] = FakeClient{true, true, false, true, false, true, "Alice", 76561198000000003ULL};

	// --- before the first update/map: not one client call may reach s2sdk ----------------------
	ResetClientCounters();
	CHECK(!CanQueryClient(3), "CanQueryClient is false before the gate opens");
	CHECK(!HasClient(3), "HasClient is false before the gate opens");
	CHECK(!HasController(3), "HasController is false before the gate opens");
	CHECK(ClientSteamID64(3) == 0, "the SteamID64 is 0 before the gate opens");
	CHECK(ClientName(3).empty(), "the name is empty before the gate opens");
	CheckEqInt(g_clientCalls, 0, "not one s2sdk client call before the gate opens");

	// A full SteamID64 from the authorization event is enough for the HTTP query even before the
	// first update (the original did the same); it must not make a client call.
	FireAuth(3, 76561198000000003ULL);
	CHECK(steamhttp::g_requests.size() == 1);
	CheckEqInt(g_clientCalls, 0, "the pre-map authorization makes no client call");
	steamhttp::g_requests.clear();

	// A punish needs the slot's SteamID64, so before the map it reports nothing.
	FirePunish(3, admin_system::RT_BAN, 60, "pre-map");
	CHECK(steamhttp::g_requests.empty(), "a pre-map punish sends nothing");
	CheckEqInt(g_clientCalls, 0, "a pre-map punish makes no client call");

	// --- the first update of a plugin loaded into a running map latches the gate --------------
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(1));
	CHECK(g_mapReady, "the first update opens the gate");
	ResetClientCounters();
	CHECK(HasClient(3), "the live client is reachable");
	CHECK(HasController(3), "the client has a controller");
	CHECK(ClientName(3) == "Alice", "the name is read");
	CHECK(ClientSteamID64(3) == 76561198000000003ULL, "the SteamID64 is read");
	CHECK(g_clientCalls > 0, "the client calls reach s2sdk after the gate opens");

	// --- OnMapEnd drops readiness; an update in the window does not reopen it -----------------
	MapEnd();
	CHECK(!g_mapReady, "OnMapEnd drops readiness");
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(!g_mapReady, "an update after OnMapEnd does not reopen the gate");
	ResetClientCounters();
	CHECK(!HasClient(3), "HasClient is false after OnMapEnd");
	CHECK(!HasController(3), "HasController is false after OnMapEnd");
	CHECK(ClientName(3).empty(), "the name is empty after OnMapEnd");
	CHECK(ClientSteamID64(3) == 0, "the SteamID64 is 0 after OnMapEnd");
	CheckEqInt(g_clientCalls, 0, "not one s2sdk client call after OnMapEnd");

	// --- the next OnMapStart opens the gate again ---------------------------------------------
	MapStart();
	CHECK(g_mapReady, "OnMapStart latches the gate again");
	CHECK(HasClient(3) && ClientName(3) == "Alice", "the client is reachable again");

	FinishTest();
}

// A plugin loaded during the map change (OnMapEnd before its first update) must not latch on the
// update: only the next OnMapStart opens the gate.
void TestMapChangeWindowStart() {
	std::printf("-- plugin loaded into the map-change window\n");

	PrepareTest();
	WriteConfig(kTestConfig);
	g_asBlockDBPlugin.OnPluginStart();
	MapEnd();
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(!g_mapReady, "the first update after an OnMapEnd does not latch the gate");

	g_clients[2] = FakeClient{true, true, false, true, false, true, "Bob", 76561198000000002ULL};
	ResetClientCounters();
	CHECK(ClientSteamID64(2) == 0, "no client id in the map-change window");
	CheckEqInt(g_clientCalls, 0, "no client call in the map-change window");

	MapStart();
	CHECK(g_mapReady, "the next OnMapStart opens the gate");
	CHECK(HasClient(2) && ClientName(2) == "Bob", "the client is reachable after the map start");

	FinishTest();
}

void TestClientGuards() {
	std::printf("-- client guards (PITFALLS C2)\n");

	PrepareTest();
	WriteConfig(kTestConfig);
	g_asBlockDBPlugin.OnPluginStart();
	MapStart();
	CHECK(g_mapReady);

	g_clients[7] = FakeClient{true, false, false, true, false, false, "Dave", 76561198000000007ULL};
	g_clients[8] = FakeClient{true, true, false, true, false, true, "", 76561198000000008ULL};
	g_clients[9] = FakeClient{true, true, true, true, false, true, "SourceTV", 0};
	g_clients[10] = FakeClient{true, true, false, false, false, true, "NoController", 76561198000000010ULL};
	g_clients[11] = FakeClient{false, false, false, true, false, false, "Gone", 76561198000000011ULL};

	CHECK(HasClient(7), "a connected client passes");
	CHECK(!HasController(10), "without a controller there is no name");
	CHECK(ClientName(10).empty(), "the name of a controllerless client is empty");
	CHECK(ClientSteamID64(7) == 76561198000000007ULL, "the id only needs the client object");
	CHECK(ClientName(7) == "Dave", "the name is read");
	CHECK(ClientName(8).empty(), "s2sdk's \"<unknown>\" becomes an empty name");
	CHECK(!HasClient(9), "the SourceTV proxy is not a player");
	CHECK(ClientSteamID64(9) == 0, "SourceTV has no SteamID64");
	CHECK(!HasClient(11), "a disconnected slot fails the guard");
	CHECK(!HasClient(-1), "a negative slot fails the guard");
	CHECK(!HasClient(64), "slot 64 fails the guard");
	CHECK(!HasController(-1), "a negative slot has no controller");
	CHECK(ClientSteamID64(-1) == 0, "a negative slot has no id");
	CHECK(ClientSteamID64(64) == 0, "slot 64 has no id");
	CHECK(ClientName(-1).empty(), "a negative slot has no name");

	// An out-of-range slot must be rejected before any s2sdk call.
	ResetClientCounters();
	CHECK(!HasClient(-1));
	CHECK(!HasClient(64));
	CheckEqInt(g_clientCalls, 0, "out-of-range slots make no client call");

	FinishTest();
}

// --- authorization ----------------------------------------------------------------------------

void TestAuthEvent() {
	std::printf("-- OnClientAuthenticated / the ban query\n");

	PrepareTest();
	WriteConfig(kTestConfig);
	g_asBlockDBPlugin.OnPluginStart();
	MapStart();
	g_clients[3] = FakeClient{true, false, false, true, false, true, "Alice", 76561198000000003ULL};

	// The event carries the SteamID64; the query uses the slot's own id and the Bearer header.
	FireAuth(3, 76561198000000003ULL);
	CHECK(steamhttp::g_requests.size() == 1);
	steamhttp::FakeRequest request = RequestAt(0);
	CHECK(request.method == steamhttp::Method::Get);
	CheckEq(request.url, "https://api.blockdb.ru/v2/bans/76561198000000003", "GET url");
	CheckEq(request.authorization, "Bearer test-api-key", "GET uses the Bearer prefix");
	CheckEq(request.contentType, "application/json", "GET content type");
	CHECK(request.body.empty(), "GET has no body");

	// Seen: the same slot+steamid is not queried twice.
	FireAuth(3, 76561198000000003ULL);
	CHECK(steamhttp::g_requests.size() == 1);

	// Canonical id overrides a 32-bit account id from the event.
	g_clients[4] = FakeClient{true, false, false, true, false, true, "Bob", 76561198000000004ULL};
	FireAuth(4, 123456);
	CHECK(steamhttp::g_requests.size() == 2);
	CheckEq(RequestAt(1).url, "https://api.blockdb.ru/v2/bans/76561198000000004", "canonical id wins");

	// No canonical id and a 32-bit id: not a SteamID64 yet - the reconcile pass picks it up.
	g_clients[5] = FakeClient{true, false, false, true, false, true, "Connecting", 0};
	FireAuth(5, 123456);
	CHECK(steamhttp::g_requests.size() == 2);

	// No canonical id but a full SteamID64 in the event: usable (HTTP only, no client call).
	ResetClientCounters();
	FireAuth(6, 76561198000000006ULL);
	CHECK(steamhttp::g_requests.size() == 3);
	CheckEq(RequestAt(2).url, "https://api.blockdb.ru/v2/bans/76561198000000006", "full event id used");
	CheckEqInt(g_steamIdCalls, 0, "an empty slot has no SteamID64 to read");
	CheckEqInt(g_controllerCalls, 0, "an empty slot has no controller to read");

	// A negative slot is rejected before any client call.
	ResetClientCounters();
	FireAuth(-1, 76561198000000009ULL);
	CHECK(steamhttp::g_requests.size() == 3);
	CheckEqInt(g_clientCalls, 0, "a negative event slot makes no client call");

	// The 60 s dedupe: the slot may have been cleared by a disconnect, the steamid still
	// suppresses a second notification.
	g_seenSteamIds[3] = 0;
	g_lastConnectTime[76561198000000003ULL] = static_cast<int64_t>(std::time(nullptr)) - 10;
	FireAuth(3, 76561198000000003ULL);
	CHECK(steamhttp::g_requests.size() == 3);

	// Entries older than the window are pruned and no longer suppress.
	const uint64_t other = 76561198000000099ULL;
	g_lastConnectTime[other] = static_cast<int64_t>(std::time(nullptr)) - 120;
	g_seenSteamIds[3] = 0;
	g_lastConnectTime.erase(76561198000000003ULL);
	FireAuth(3, 76561198000000003ULL);
	CHECK(steamhttp::g_requests.size() == 4);
	CHECK(g_lastConnectTime.find(other) == g_lastConnectTime.end(), "an expired entry is pruned");

	FinishTest();
}

// --- the BlockDB API: punish / unpunish -------------------------------------------------------

void TestPunishUnpunish() {
	std::printf("-- ban / unban requests\n");

	PrepareTest();
	WriteConfig(kTestConfig);
	g_asBlockDBPlugin.OnPluginStart();
	MapStart();
	g_clients[2] = FakeClient{true, true, false, true, false, true, "Carol", 76561198000000002ULL};

	// POST /v2/bans with the ApiKey (no Bearer) and the three body fields.
	FirePunish(2, admin_system::RT_BAN, 600, "wallhack");
	CHECK(steamhttp::g_requests.size() == 1);
	const steamhttp::FakeRequest ban = RequestAt(0);
	CHECK(ban.method == steamhttp::Method::Post);
	CheckEq(ban.url, "https://api.blockdb.ru/v2/bans", "ban URL");
	CheckEq(ban.authorization, "test-api-key", "ban uses the plain ApiKey");
	CheckEq(ban.contentType, "application/json", "ban content type");
	CheckEq(JsonField(ban.body, "steamid64"), "76561198000000002", "ban steamid64 is a number");
	CheckEq(JsonField(ban.body, "reason"), "wallhack", "ban reason");
	CheckEq(JsonField(ban.body, "duration"), "600", "ban duration");

	// Only RT_BAN is reported.
	FirePunish(2, admin_system::RT_MUTE, 60, "mute");
	FirePunish(2, admin_system::RT_GAG, 60, "gag");
	FirePunish(2, admin_system::RT_SILENCE, 60, "silence");
	CHECK(steamhttp::g_requests.size() == 1, "other punishment types are not reported");

	// Negative/out-of-range slots are rejected before any client call; an empty (disconnected)
	// slot is probed by the guard but never yields a SteamID64.
	ResetClientCounters();
	FirePunish(-1, admin_system::RT_BAN, 10, "x");
	FirePunish(64, admin_system::RT_BAN, 10, "x");
	CheckEqInt(g_clientCalls, 0, "invalid slots make no client call");
	FirePunish(5, admin_system::RT_BAN, 10, "x");
	CheckEqInt(g_steamIdCalls, 0, "an empty slot has no SteamID64");
	CHECK(steamhttp::g_requests.size() == 1);

	// A null reason is reported as an empty string, like the original.
	FirePunish(2, admin_system::RT_BAN, 0, nullptr);
	CHECK(steamhttp::g_requests.size() == 2);
	CheckEq(JsonField(RequestAt(1).body, "reason"), "", "null reason -> empty string");
	CheckEq(JsonField(RequestAt(1).body, "duration"), "0", "permanent ban duration 0");

	// DELETE /v2/bans with the steamid as a string.
	FireUnpunish("76561198000000002", admin_system::RT_BAN);
	CHECK(steamhttp::g_requests.size() == 3);
	const steamhttp::FakeRequest unban = RequestAt(2);
	CHECK(unban.method == steamhttp::Method::Delete);
	CheckEq(unban.url, "https://api.blockdb.ru/v2/bans", "unban URL");
	CheckEq(unban.authorization, "test-api-key", "unban uses the plain ApiKey");
	CheckEq(JsonField(unban.body, "steamid64"), "76561198000000002", "unban steamid64 is a string");

	// Other types and empty/null ids are ignored.
	FireUnpunish("76561198000000002", admin_system::RT_MUTE);
	FireUnpunish("", admin_system::RT_BAN);
	FireUnpunish(nullptr, admin_system::RT_BAN);
	CHECK(steamhttp::g_requests.size() == 3);

	// Order: the requests went out in the order the admin core called back.
	CHECK(RequestAt(0).method == steamhttp::Method::Post);
	CHECK(RequestAt(1).method == steamhttp::Method::Post);
	CHECK(RequestAt(2).method == steamhttp::Method::Delete);

	FinishTest();
}

// --- the BlockDB API: the ban query responses -------------------------------------------------

constexpr uint64_t kBanSteam = 76561198000000003ULL;

nlohmann::json OneBan(uint64_t discordId, const char* reason = "cheat", const char* project = "MIRAGE") {
	return nlohmann::json::array({{{"discord_id", discordId},
								   {"reason", reason},
								   {"project_name", project},
								   {"created_at", 1700000000}}});
}

nlohmann::json ThreeDistinctBans() {
	return nlohmann::json::array({{{"discord_id", 1}}, {{"discord_id", 2}}, {{"discord_id", 3}}});
}

bool DeliverBanResponse(std::size_t requestIndex, const nlohmann::json& bans) {
	return steamhttp::Deliver(requestIndex, steamhttp::Response{true, true, 200,
																R"({"bans":)" + bans.dump() + "}"});
}

void TestBanQueryResponses() {
	std::printf("-- ban query responses and errors\n");

	PrepareTest();
	WriteConfig(kTestConfig);
	g_asBlockDBPlugin.OnPluginStart();
	MapStart();
	g_clients[3] = FakeClient{true, true, false, true, false, true, "Alice", kBanSteam};

	// Transport failure: logged, no webhook, no kick.
	steamhttp::g_requests.clear();
	HandleBanQueryResponse(steamhttp::Response{false, false, 0, ""}, 3, kBanSteam, "Alice");
	CHECK(Logged(plg::Severity::Error, "GET 76561198000000003 failed (no response"));
	CHECK(steamhttp::g_requests.empty());
	CHECK(g_kicks.empty());

	// A non-2xx status: logged, no webhook, no kick.
	HandleBanQueryResponse(steamhttp::Response{true, false, 500, ""}, 3, kBanSteam, "Alice");
	CHECK(Logged(plg::Severity::Error, "failed with status code 500"));
	CHECK(steamhttp::g_requests.empty());

	// Malformed JSON: logged, no webhook.
	HandleBanQueryResponse(steamhttp::Response{true, true, 200, "not json"}, 3, kBanSteam, "Alice");
	CHECK(Logged(plg::Severity::Error, "Failed to parse the JSON of the BlockDB response"));
	CHECK(steamhttp::g_requests.empty());

	// No "bans", an empty list, a non-array: nothing happens.
	HandleBanQueryResponse(steamhttp::Response{true, true, 200, R"({"bans":[]})"}, 3, kBanSteam, "Alice");
	HandleBanQueryResponse(steamhttp::Response{true, true, 200, R"({"bans":"x"})"}, 3, kBanSteam, "Alice");
	HandleBanQueryResponse(steamhttp::Response{true, true, 200, R"({})"}, 3, kBanSteam, "Alice");
	CHECK(steamhttp::g_requests.empty());
	CHECK(g_kicks.empty());

	// One offence and the threshold is 3: a webhook report with the three fields.
	steamhttp::g_requests.clear();
	const auto one = OneBan(111).dump();
	HandleBanQueryResponse(steamhttp::Response{true, true, 200, R"({"bans":)" + one + "}"}, 3, kBanSteam,
						   "Alice");
	CHECK(steamhttp::g_requests.size() == 1);
	if (steamhttp::g_requests.size() == 1) {
		const steamhttp::FakeRequest report = RequestAt(0);
		CHECK(report.method == steamhttp::Method::Post);
		CheckEq(report.url, "https://discord.invalid/webhook", "webhook URL");
		CheckEq(report.authorization, "", "the webhook carries no Authorization header");
		const nlohmann::json parsed = nlohmann::json::parse(report.body, nullptr, false);
		CHECK(!parsed.is_discarded());
		CHECK(parsed.contains("embeds") && parsed["embeds"].is_array() && parsed["embeds"].size() == 1);
		if (parsed.contains("embeds") && parsed["embeds"].is_array() && !parsed["embeds"].empty()) {
			const nlohmann::json& embed = parsed["embeds"][0];
			CheckEq(embed["title"].get<std::string>(), "Новое подключение нарушителя", "embed title");
			CHECK(embed["fields"].size() == 3);
			CHECK(embed["fields"][0]["value"].get<std::string>().find("Alice") != std::string::npos);
			CHECK(embed["fields"][0]["value"].get<std::string>().find("steamcommunity.com/profiles/76561198000000003") !=
				  std::string::npos);
			CheckEq(embed["fields"][1]["value"].get<std::string>(), "```76561198000000003```", "steamid field");
			CHECK(embed["fields"][2]["name"].get<std::string>().find("(1)") != std::string::npos);
			CHECK(embed["fields"][2]["value"].get<std::string>().find("cheat") != std::string::npos);
			CHECK(embed["fields"][2]["value"].get<std::string>().find("MIRAGE") != std::string::npos);
			CHECK(embed["footer"]["text"].get<std::string>().find("Test Server | ") == 0);
		}
	}
	CHECK(g_kicks.empty());

	// Three different discord ids reach the threshold: kick, no webhook.
	steamhttp::g_requests.clear();
	g_kicks.clear();
	const auto three = nlohmann::json::array({{{"discord_id", 1}}, {{"discord_id", 2}}, {{"discord_id", 3}}}).dump();
	HandleBanQueryResponse(steamhttp::Response{true, true, 200, R"({"bans":)" + three + "}"}, 3, kBanSteam,
						   "Alice");
	CHECK(steamhttp::g_requests.empty(), "a kick reports nothing to the webhook");
	CHECK(g_kicks.size() == 1);
	if (!g_kicks.empty()) {
		CheckEqInt(g_kicks[0].slot, 3, "the kick goes to the queried slot");
		CHECK(g_kicks[0].reason == s2sdk::NetworkDisconnectionReason::Kickbanadded);
	}

	// Two bans with the same discord id count as one distinct: below the threshold.
	steamhttp::g_requests.clear();
	g_kicks.clear();
	const auto same = nlohmann::json::array({{{"discord_id", 7}}, {{"discord_id", 7}}}).dump();
	HandleBanQueryResponse(steamhttp::Response{true, true, 200, R"({"bans":)" + same + "}"}, 3, kBanSteam,
						   "Alice");
	CHECK(g_kicks.empty());
	CHECK(steamhttp::g_requests.size() == 1, "below the threshold the report is posted");

	// The slot changed hands: the kick for the old steamid is skipped (the original would have
	// kicked the newcomer), and the threshold branch does not report either.
	g_clients[3].steamid64 = 76561198000000055ULL;
	steamhttp::g_requests.clear();
	g_kicks.clear();
	HandleBanQueryResponse(steamhttp::Response{true, true, 200, R"({"bans":)" + three + "}"}, 3, kBanSteam,
						   "Alice");
	CHECK(g_kicks.empty(), "no kick after a slot change");
	CHECK(steamhttp::g_requests.empty());

	// AutoBlockBanCount 0 reproduces the original: one distinct id is enough to kick.
	g_clients[3].steamid64 = kBanSteam;
	g_config.autoBlockBanCount = 0;
	steamhttp::g_requests.clear();
	g_kicks.clear();
	HandleBanQueryResponse(steamhttp::Response{true, true, 200, R"({"bans":)" + one + "}"}, 3, kBanSteam,
						   "Alice");
	CHECK(g_kicks.size() == 1, "count 0 kicks on the first distinct id");
	CHECK(steamhttp::g_requests.empty());
	g_config.autoBlockBanCount = 3;

	// Broken UTF-8 in the reason and in the player name must not throw; the report is still sent.
	g_clients[3].steamid64 = kBanSteam;
	steamhttp::g_requests.clear();
	const std::string badBytes = "\xFF\xFE" "cheat";
	const auto broken = nlohmann::json::array(
		{{{"discord_id", 1}, {"reason", badBytes}, {"project_name", badBytes}, {"created_at", 1700000000}}})
							.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
	const std::string playerName = "bad\xFFname";
	CHECK_NOTHROW(HandleBanQueryResponse(steamhttp::Response{true, true, 200, R"({"bans":)" + broken + "}"},
										 3, kBanSteam, playerName));
	CHECK(steamhttp::g_requests.size() == 1);
	CHECK(!nlohmann::json::parse(RequestAt(0).body, nullptr, false).is_discarded());

	// Counts ignore broken entries: non-objects, null/non-number discord ids.
	const nlohmann::json bans = nlohmann::json::parse(
		R"([{"discord_id":1},{"discord_id":null},{"discord_id":"3"},5,[],{"discord_id":1}])");
	const auto counts = GetCounts(bans);
	CheckEqInt(counts.first, 6, "every element is a ban");
	CheckEqInt(counts.second, 1, "only distinct numeric discord ids");

	FinishTest();
}

// --- deferred kick decisions across the map gate ------------------------------------------------

void TestPendingKick() {
	std::printf("-- pending kick across OnMapEnd/OnMapStart\n");

	// 1. A ban-list answer that arrives before the map must not be lost: no kick and no s2sdk
	// call while the gate is closed, then the same slot+steamid is kicked after OnMapStart.
	PrepareTest();
	WriteConfig(kTestConfig);
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	g_clients[3] = FakeClient{true, false, false, true, false, true, "Alice", kBanSteam};

	FireAuth(3, kBanSteam);
	CHECK(steamhttp::g_requests.size() == 1);
	const std::size_t preMapQuery = steamhttp::g_requests.size() - 1;
	ResetClientCounters();
	CHECK(DeliverBanResponse(preMapQuery, ThreeDistinctBans()));
	CHECK(g_pendingKicks[3].active, "the kick decision is stored");
	CHECK(g_pendingKicks[3].steamid == kBanSteam, "the decision keeps the steamid");
	CHECK(g_kicks.empty(), "nothing is kicked while the map gate is closed");
	CheckEqInt(g_clientCalls, 0, "no engine call while the map gate is closed");
	TryResolvePendingKicks(); // even the explicit retry must not touch s2sdk before OnMapStart
	CheckEqInt(g_clientCalls, 0, "the closed-gate retry makes no engine call");
	CHECK(g_pendingKicks[3].active, "the closed-gate retry keeps the decision");
	CHECK(steamhttp::g_requests.size() == 1, "no second GET and no webhook");

	MapStart();
	g_sinceReconcile = std::chrono::milliseconds(0); // isolate from the reconcile pass
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.size() == 1, "the pending kick is executed after OnMapStart");
	if (!g_kicks.empty()) {
		CheckEqInt(g_kicks[0].slot, 3, "the kick goes to the stored slot");
		CHECK(g_kicks[0].reason == s2sdk::NetworkDisconnectionReason::Kickbanadded);
	}
	CHECK(!g_pendingKicks[3].active, "the decision is consumed");
	CHECK(steamhttp::g_requests.size() == 1, "the retry sends nothing");

	// 2. Slot reuse: the pending decision for the old steamid must never kick the newcomer.
	PrepareTest();
	WriteConfig(kTestConfig);
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	g_clients[4] = FakeClient{true, false, false, true, false, true, "Bob", kBanSteam + 1};
	FireAuth(4, kBanSteam + 1);
	const std::size_t reuseQuery = steamhttp::g_requests.size() - 1;
	CHECK(DeliverBanResponse(reuseQuery, ThreeDistinctBans()));
	CHECK(g_pendingKicks[4].active);
	MapStart();
	g_clients[4].steamid64 = kBanSteam + 2; // a different player in the same slot
	g_sinceReconcile = std::chrono::milliseconds(0);
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.empty(), "a recycled slot is never kicked");
	CHECK(!g_pendingKicks[4].active, "the stale decision is dropped on reuse");
	CHECK(steamhttp::g_requests.size() == 1, "reuse does not send a GET or a webhook");

	// 3. A temporarily unreadable client keeps the decision until it becomes readable again
	// (disconnected, then connected but not authenticated yet), and then the kick lands.
	PrepareTest();
	WriteConfig(kTestConfig);
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	g_clients[5] = FakeClient{true, false, false, true, false, true, "Carol", kBanSteam + 3};
	FireAuth(5, kBanSteam + 3);
	const std::size_t waitQuery = steamhttp::g_requests.size() - 1;
	CHECK(DeliverBanResponse(waitQuery, ThreeDistinctBans()));
	CHECK(g_pendingKicks[5].active);
	MapStart();

	g_clients[5].connected = false;
	g_sinceReconcile = std::chrono::milliseconds(0);
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_pendingKicks[5].active, "a disconnected slot keeps the decision");
	CHECK(g_kicks.empty());

	g_clients[5].connected = true;
	g_clients[5].steamid64 = 0; // connected but not authenticated yet
	g_sinceReconcile = std::chrono::milliseconds(0);
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_pendingKicks[5].active, "an unauthenticated client keeps the decision");
	CHECK(g_kicks.empty());

	g_clients[5].steamid64 = kBanSteam + 3;
	g_sinceReconcile = std::chrono::milliseconds(0);
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.size() == 1, "the kick lands once the same player is readable");
	CHECK(!g_pendingKicks[5].active);
	CHECK(steamhttp::g_requests.size() == 1);

	// 4. The short expiry drops a decision for a player who never becomes readable.
	PrepareTest();
	WriteConfig(kTestConfig);
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	g_clients[6] = FakeClient{true, false, false, true, false, true, "Dave", kBanSteam + 4};
	FireAuth(6, kBanSteam + 4);
	const std::size_t expiryQuery = steamhttp::g_requests.size() - 1;
	CHECK(DeliverBanResponse(expiryQuery, ThreeDistinctBans()));
	CHECK(g_pendingKicks[6].active);
	MapStart();
	g_pendingKicks[6].expiry = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
	g_sinceReconcile = std::chrono::milliseconds(0);
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(!g_pendingKicks[6].active, "an expired decision is dropped");
	CHECK(g_kicks.empty(), "an expired decision never kicks");
	CHECK(steamhttp::g_requests.size() == 1, "expiry sends nothing");

	// 5. OnPluginEnd clears every pending decision before the library goes away.
	PrepareTest();
	WriteConfig(kTestConfig);
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	g_clients[7] = FakeClient{true, false, false, true, false, true, "Eve", kBanSteam + 5};
	FireAuth(7, kBanSteam + 5);
	const std::size_t teardownQuery = steamhttp::g_requests.size() - 1;
	CHECK(DeliverBanResponse(teardownQuery, ThreeDistinctBans()));
	CHECK(g_pendingKicks[7].active);
	CHECK(g_asBlockDBPlugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok);
	CHECK(!g_pendingKicks[7].active, "OnPluginEnd clears pending decisions");
	CHECK(g_kicks.empty());
	CHECK(steamhttp::g_requests.size() == 1);

	FinishTest();
}

// --- pending kick safety: fake/unauthorized/missing bindings ----------------------------------

void TestPendingKickSafetyGuards() {
	std::printf("-- pending kick requires IsFakeClient + IsClientAuthorized\n");

	// Queues a pending decision for `slot` before the map, then opens the map. No resolver run yet.
	const auto queueAndStart = [](int32_t slot, uint64_t steamid, bool fake, bool authorized) {
		PrepareTest();
		WriteConfig(kTestConfig);
		g_asBlockDBPlugin.OnPluginStart();
		g_clients[static_cast<std::size_t>(slot)] =
			FakeClient{true, false, false, true, fake, authorized, "Pending", steamid};
		FireAuth(slot, steamid);
		const std::size_t index = steamhttp::g_requests.size() - 1;
		const bool delivered = DeliverBanResponse(index, ThreeDistinctBans());
		MapStart();
		g_sinceReconcile = std::chrono::milliseconds(0);
		return delivered;
	};

	// 1. IsFakeClient binding missing: a normal player must not be kicked until it is back.
	CHECK(queueAndStart(8, kBanSteam + 10, false, true));
	CHECK(g_pendingKicks[8].active);
	const auto savedFake = __s2sdk_IsFakeClient;
	__s2sdk_IsFakeClient = nullptr;
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.empty(), "no kick without the IsFakeClient binding");
	CHECK(g_pendingKicks[8].active, "the decision is held until the binding is back");
	__s2sdk_IsFakeClient = savedFake;
	g_sinceReconcile = std::chrono::milliseconds(0);
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.size() == 1, "the kick lands after IsFakeClient is restored");
	CHECK(!g_pendingKicks[8].active);
	CHECK(steamhttp::g_requests.size() == 1, "no GET/webhook on the guarded retry");

	// 2. The same, but the slot holds a fake client with a non-zero SteamID: while the binding is
	// missing it must be held (never kicked), and once restored it is cleared as a fake.
	CHECK(queueAndStart(9, kBanSteam + 11, true, true));
	CHECK(g_pendingKicks[9].active);
	__s2sdk_IsFakeClient = nullptr;
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.empty(), "a fake client is not kicked without the IsFakeClient binding");
	CHECK(g_pendingKicks[9].active);
	__s2sdk_IsFakeClient = savedFake;
	g_sinceReconcile = std::chrono::milliseconds(0);
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.empty(), "a fake client is never kicked");
	CHECK(!g_pendingKicks[9].active, "the decision for a fake client is dropped");
	CHECK(steamhttp::g_requests.size() == 1);

	// 3. IsClientAuthorized binding missing: hold; restore -> kick.
	CHECK(queueAndStart(10, kBanSteam + 12, false, true));
	CHECK(g_pendingKicks[10].active);
	const auto savedAuthorized = __s2sdk_IsClientAuthorized;
	__s2sdk_IsClientAuthorized = nullptr;
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.empty(), "no kick without the IsClientAuthorized binding");
	CHECK(g_pendingKicks[10].active, "the decision is held until the binding is back");
	__s2sdk_IsClientAuthorized = savedAuthorized;
	g_sinceReconcile = std::chrono::milliseconds(0);
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.size() == 1, "the kick lands after IsClientAuthorized is restored");
	CHECK(!g_pendingKicks[10].active);
	CHECK(steamhttp::g_requests.size() == 1);

	// 4. Connected but not authorized yet, with a non-zero (unauthenticated) SteamID that already
	// matches the decision: hold, never kick; after authorization -> kick.
	CHECK(queueAndStart(11, kBanSteam + 13, false, false));
	CHECK(g_pendingKicks[11].active);
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.empty(), "an unauthorized client is never kicked");
	CHECK(g_pendingKicks[11].active, "the decision is held while unauthorized");
	g_clients[11].authorized = true;
	g_sinceReconcile = std::chrono::milliseconds(0);
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.size() == 1, "the kick lands after authorization");
	CHECK(!g_pendingKicks[11].active);
	CHECK(steamhttp::g_requests.size() == 1);

	// 5. Both bindings present and the slot is a fake client with a non-zero SteamID: cleared,
	// never kicked, and never applied to another slot.
	CHECK(queueAndStart(12, kBanSteam + 14, true, true));
	CHECK(g_pendingKicks[12].active);
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.empty(), "a fake client with a non-zero SteamID is never kicked");
	CHECK(!g_pendingKicks[12].active, "the fake-client decision is dropped");
	CHECK(steamhttp::g_requests.size() == 1);

	// 6. A decision is bound to its own slot: the same SteamID in another slot must not receive
	// the kick while the original slot is temporarily unreadable; it lands on the original slot.
	CHECK(queueAndStart(13, kBanSteam + 15, false, true));
	CHECK(g_pendingKicks[13].active);
	g_clients[13].connected = false;
	g_clients[14] = FakeClient{true, false, false, true, false, true, "Other", kBanSteam + 15};
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.empty(), "another slot with the same SteamID is not kicked");
	CHECK(g_pendingKicks[13].active);
	CHECK(!g_pendingKicks[14].active);
	g_clients[13].connected = true;
	g_clients[13].steamid64 = kBanSteam + 15;
	g_sinceReconcile = std::chrono::milliseconds(0);
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(g_kicks.size() == 1, "the kick belongs to the original slot");
	if (!g_kicks.empty()) {
		CheckEqInt(g_kicks[0].slot, 13, "the kick slot is the pending slot");
	}
	CHECK(steamhttp::g_requests.size() == 1);

	FinishTest();
}

// --- request ordering end to end ---------------------------------------------------------------

void TestRequestOrder() {
	std::printf("-- request order\n");

	PrepareTest();
	WriteConfig(kTestConfig);
	g_asBlockDBPlugin.OnPluginStart();
	MapStart();
	g_clients[1] = FakeClient{true, true, false, true, false, true, "Dave", 76561198000000001ULL};
	g_clients[2] = FakeClient{true, true, false, true, false, true, "Eve", 76561198000000002ULL};

	FirePunish(1, admin_system::RT_BAN, 60, "aim");
	FireUnpunish("76561198000000009", admin_system::RT_BAN);
	FireAuth(2, 76561198000000002ULL);

	CHECK(steamhttp::g_requests.size() == 3);
	CHECK(RequestAt(0).method == steamhttp::Method::Post);
	CheckEq(RequestAt(0).url, "https://api.blockdb.ru/v2/bans", "order: ban POST");
	CHECK(RequestAt(1).method == steamhttp::Method::Delete);
	CheckEq(RequestAt(1).url, "https://api.blockdb.ru/v2/bans", "order: unban DELETE");
	CHECK(RequestAt(2).method == steamhttp::Method::Get);
	CheckEq(RequestAt(2).url, "https://api.blockdb.ru/v2/bans/76561198000000002", "order: auth GET");

	// Completing the GET adds the webhook request at the end.
	const auto one = OneBan(5).dump();
	CHECK(steamhttp::Deliver(2, steamhttp::Response{true, true, 200, R"({"bans":)" + one + "}"}));
	CHECK(steamhttp::g_requests.size() == 4);
	CHECK(RequestAt(3).method == steamhttp::Method::Post);
	CheckEq(RequestAt(3).url, "https://discord.invalid/webhook", "order: webhook last");

	FinishTest();
}

// --- reconcile ---------------------------------------------------------------------------------

void ReconcileTick(int64_t ms = 1000) {
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(ms));
}

void TestReconcile() {
	std::printf("-- the once-per-second reconcile\n");

	PrepareTest();
	WriteConfig(kTestConfig);
	g_asBlockDBPlugin.OnPluginStart();
	MapStart();

	// The first pass only seeds: a player who is already online is not re-checked (the original
	// never re-checked players that were already connected).
	g_clients[1] = FakeClient{true, true, false, true, false, true, "Alice", 76561198000000001ULL};
	ResetClientCounters();
	ReconcileTick();
	CHECK(steamhttp::g_requests.empty(), "the first reconcile pass only records who is online");
	CHECK(g_seenSeeded);
	CHECK(g_seenSteamIds[1] == 76561198000000001ULL);
	CHECK(g_clientCalls > 0, "the seeding pass did query the connected clients");

	// A player who appears after the seed is queried by the next pass, once.
	g_clients[2] = FakeClient{true, true, false, true, false, true, "Bob", 76561198000000002ULL};
	ReconcileTick();
	CHECK(steamhttp::g_requests.size() == 1);
	CheckEq(RequestAt(0).url, "https://api.blockdb.ru/v2/bans/76561198000000002", "new player query");
	ReconcileTick();
	CHECK(steamhttp::g_requests.size() == 1, "the same player is not queried again");

	// Bots and unauthorized clients are skipped.
	g_clients[3] = FakeClient{true, true, false, true, true, true, "BOT", 0};
	ReconcileTick();
	CHECK(steamhttp::g_requests.size() == 1, "a bot is skipped");
	g_clients[4] = FakeClient{true, true, false, true, false, false, "NotAuth", 76561198000000004ULL};
	ReconcileTick();
	CHECK(steamhttp::g_requests.size() == 1, "an unauthorized client is skipped");

	// A slot that empties loses its recorded steamid; a new occupant is handled.
	g_clients[2] = FakeClient{};
	ReconcileTick();
	CHECK(g_seenSteamIds[2] == 0, "a disconnected slot is cleared");
	g_clients[2] = FakeClient{true, true, false, true, false, true, "New", 76561198000000022ULL};
	ReconcileTick();
	CHECK(steamhttp::g_requests.size() == 2);
	CheckEq(RequestAt(1).url, "https://api.blockdb.ru/v2/bans/76561198000000022", "the new occupant");

	// While the gate is closed the reconcile makes no client call at all.
	MapEnd();
	CHECK(g_seenSteamIds[1] == 0 && g_seenSteamIds[2] == 0,
		  "OnMapEnd clears every per-slot steamid (state only)");
	ResetClientCounters();
	ReconcileTick();
	CheckEqInt(g_clientCalls, 0, "no client call from the reconcile between maps");
	CHECK(!g_mapReady);

	FinishTest();
}

// --- the player_disconnect game event ---------------------------------------------------------

void TestDisconnectEvent() {
	std::printf("-- player_disconnect / GetEventPlayerSlot\n");

	PrepareTest();
	WriteConfig(kTestConfig);
	g_asBlockDBPlugin.OnPluginStart();
	MapStart();
	g_clients[3] = FakeClient{true, true, false, true, false, true, "Alice", kBanSteam};

	FireAuth(3, kBanSteam);
	CHECK(steamhttp::g_requests.size() == 1);
	CHECK(g_seenSteamIds[3] == kBanSteam);

	// The handler resolves the player-typed "userid" field and clears the slot state.
	CHECK(FireDisconnect(3) == s2sdk::ResultType::Continue);
	CheckEq(g_eventKey, "userid", "the player-typed field is userid");
	CHECK(g_seenSteamIds[3] == 0, "the disconnect clears the slot");
	CHECK(steamhttp::g_requests.size() == 1, "the disconnect sends nothing");

	// A negative resolved slot is ignored, never used as an index.
	g_seenSteamIds[0] = 42;
	CHECK(FireDisconnect(-1) == s2sdk::ResultType::Continue);
	CHECK(g_seenSteamIds[0] == 42, "a negative slot changes nothing");
	CHECK(FireDisconnect(64) == s2sdk::ResultType::Continue);
	CHECK(g_seenSteamIds[0] == 42, "slot 64 changes nothing");

	// A null event is harmless.
	if (g_disconnectHook != nullptr) {
		CHECK(g_disconnectHook(plg::string("player_disconnect"), nullptr, false) ==
			  s2sdk::ResultType::Continue);
	}
	CHECK(g_eventSlotCalls > 0);

	FinishTest();
}

// --- degradation: no bindings, no admin core ------------------------------------------------

void TestMissingBindings() {
	std::printf("-- zero / missing bindings\n");

	// 1. The s2sdk map/auth/event bindings are missing: the plugin starts, but keeps the client
	// gate shut forever and registers nothing it could not remove.
	PrepareTest();
	WriteConfig(kTestConfig);
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	__s2sdk_OnClientAuthenticated_Register = nullptr;
	__s2sdk_OnClientAuthenticated_Unregister = nullptr;
	__s2sdk_HookEvent = nullptr;
	__s2sdk_UnhookEvent = nullptr;
	g_logs.clear();

	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart survives missing s2sdk bindings");
	CHECK(!g_mapListeners);
	CHECK(!g_authListener);
	CHECK(!g_disconnectHooked);
	CHECK(Logged(plg::Severity::Error, "the s2sdk map listeners are unavailable"));
	CHECK(Logged(plg::Severity::Error, "OnClientAuthenticated is unavailable"));
	CHECK(Logged(plg::Severity::Error, "the s2sdk event bindings are unavailable"));
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(1000));
	CHECK(!g_mapReady, "the gate can never open without the map listeners");
	ResetClientCounters();
	CHECK(ClientSteamID64(1) == 0, "no client id without the bindings");
	CheckEqInt(g_clientCalls, 0, "no client call without the bindings");
	g_asBlockDBPlugin.OnPluginEnd();
	CheckEqInt(g_mapStartUnregisters, 0, "no map start unregister without a registration");
	CheckEqInt(g_mapEndUnregisters, 0, "no map end unregister without a registration");
	CheckEqInt(g_authUnregisters, 0, "no auth unregister without a registration");
	CheckEqInt(g_unhookEventCalls, 0, "no unhook without a hook");

	// 2. A hook the event system refuses is logged, not fatal.
	InstallFakes();
	PrepareTest();
	WriteConfig(kTestConfig);
	g_hookEventResult = s2sdk::EventHookError::InvalidEvent;
	g_logs.clear();
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	CHECK(!g_disconnectHooked);
	CHECK(Logged(plg::Severity::Error, "could not hook \"player_disconnect\""));
	CHECK(g_authListener && g_mapListeners, "the other listeners are still registered");
	g_asBlockDBPlugin.OnPluginEnd();

	// 3. The admin core bindings are missing: registration is a local no-op, the callbacks are
	// still cleared on unload.
	InstallFakes();
	PrepareTest();
	WriteConfig(kTestConfig);
	__admin_system_GetPluginVersion = nullptr;
	__admin_system_OnPlayerPunish_Register = nullptr;
	__admin_system_OnOfflinePlayerUnpunish_Register = nullptr;
	__admin_system_OnPlayerPunish_Unregister = nullptr;
	__admin_system_OnOfflinePlayerUnpunish_Unregister = nullptr;
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	CheckEqInt(g_punishRegisters, 0, "no admin register without the bindings");
	CHECK(g_pAdminApi != nullptr, "the consumer wrapper is still the object every module uses");
	g_asBlockDBPlugin.OnPluginEnd();
	CHECK(g_pAdminApi == nullptr);
	CHECK(admin_system::detail::S().playerPunish == nullptr, "the stored callback is cleared on unload");
	CHECK(admin_system::detail::S().offlinePlayerUnpunish == nullptr);

	// 4. Every s2sdk binding null: a complete no-op start/update/end.
	InstallFakes();
	PrepareTest();
	WriteConfig(kTestConfig);
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	__s2sdk_OnClientAuthenticated_Register = nullptr;
	__s2sdk_OnClientAuthenticated_Unregister = nullptr;
	__s2sdk_HookEvent = nullptr;
	__s2sdk_UnhookEvent = nullptr;
	__s2sdk_GetEventPlayerSlot = nullptr;
	__s2sdk_IsClientConnected = nullptr;
	__s2sdk_IsClientSourceTV = nullptr;
	__s2sdk_PlayerSlotToEntPointer = nullptr;
	__s2sdk_GetClientSteamID64 = nullptr;
	__s2sdk_GetClientName = nullptr;
	__s2sdk_IsFakeClient = nullptr;
	__s2sdk_IsClientAuthorized = nullptr;
	__s2sdk_KickClient = nullptr;
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(1000));
	FireAuth(1, kBanSteam); // no listener was registered: nothing happens
	g_asBlockDBPlugin.OnPluginEnd();
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	g_asBlockDBPlugin.OnPluginEnd();

	// 5. Even the host logging/config callbacks missing must not crash the lifecycle (every use is
	// guarded); nothing is logged, the config keeps its defaults.
	InstallFakes();
	PrepareTest();
	WriteConfig(kTestConfig);
	plg::Log = nullptr;
	plg::GetConfigsDir = nullptr;
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	CheckEq(g_config.serverName, "", "defaults without the host callbacks");
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	g_asBlockDBPlugin.OnPluginEnd();

	InstallFakes();
	FinishTest();
}

// --- unload and detached callbacks ------------------------------------------------------------

void TestUnloadAndDetached() {
	std::printf("-- OnPluginEnd removes everything / detached callbacks\n");

	PrepareTest();
	WriteConfig(kTestConfig);
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	MapStart();
	g_clients[3] = FakeClient{true, true, false, true, false, true, "Alice", kBanSteam};

	// A query is in flight when the plugin unloads.
	FireAuth(3, kBanSteam);
	CHECK(steamhttp::g_requests.size() == 1);
	CHECK(steamhttp::PendingCount() == 1);
	const std::size_t queryIndex = steamhttp::LastRequest();
	const auto detachedHandler = steamhttp::PendingHandler(queryIndex);
	CHECK(static_cast<bool>(detachedHandler));

	// Save every callback a stale caller could still hold.
	const s2sdk::OnMapStartCallback savedMapStart = g_onMapStart;
	const s2sdk::OnMapEndCallback savedMapEnd = g_onMapEnd;
	const s2sdk::OnClientAuthenticatedCallback savedAuth = g_authCallback;
	const s2sdk::EventCallback savedDisconnect = g_disconnectHook;
	const admin_system::PlayerPunishCallback savedPunish = g_punishCallback;
	const admin_system::OfflinePlayerUnpunishCallback savedUnpunish = g_unpunishCallback;
	CHECK(savedMapStart && savedMapEnd && savedAuth && savedDisconnect && savedPunish && savedUnpunish);

	CHECK(g_asBlockDBPlugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok);

	// Symmetric cleanup (PITFALLS A7).
	CheckEqInt(g_mapStartUnregisters, 1, "the map start listener is removed");
	CheckEqInt(g_mapEndUnregisters, 1, "the map end listener is removed");
	CheckEqInt(g_authUnregisters, 1, "the auth listener is removed");
	CheckEqInt(g_unhookEventCalls, 1, "the event hook is removed");
	CheckEq(g_unhookedEvents.empty() ? "" : g_unhookedEvents[0], "player_disconnect", "unhooked event");
	CHECK(g_unhookedModes.size() == 1 && g_unhookedModes[0] == s2sdk::HookMode::Pre,
		  "the event hook is unhooked with the same Pre mode");
	CheckEqInt(g_punishUnregisters, 1, "OnPlayerPunish is unregistered");
	CheckEqInt(g_unpunishUnregisters, 1, "OnOfflinePlayerUnpunish is unregistered");
	CHECK(g_onMapStart == nullptr && g_onMapEnd == nullptr && g_authCallback == nullptr);
	CHECK(g_disconnectHook == nullptr && g_punishCallback == nullptr && g_unpunishCallback == nullptr);
	CheckEqInt(steamhttp::g_shutdownCalls, 1, "steamhttp::Shutdown is called");
	CHECK(steamhttp::PendingCount() == 0, "pending completions are dropped");
	CHECK(!g_active && !g_mapReady);
	CHECK(g_pAdminApi == nullptr);
	CHECK(g_seenSteamIds[3] == 0 && g_lastConnectTime.empty() && !g_seenSeeded);

	// A stale callback only clears/does nothing: no client query, no request.
	ResetClientCounters();
	savedMapStart();
	CHECK(!g_mapReady, "a stale OnMapStart does not reopen the gate");
	savedMapEnd();
	savedAuth(3, kBanSteam);
	CHECK(savedDisconnect(plg::string("player_disconnect"), &g_fakeEventStorage, false) ==
		  s2sdk::ResultType::Continue);
	savedPunish(3, admin_system::RT_BAN, 60, plg::string("late"), -1);
	savedUnpunish(plg::string("76561198000000003"), admin_system::RT_BAN, -1);
	CheckEqInt(g_clientCalls, 0, "a stale callback makes no client call");
	CHECK(steamhttp::g_requests.size() == 1, "a stale callback sends nothing");

	// A completion handler that escaped the transport must also be harmless after unload.
	detachedHandler(steamhttp::Response{true, true, 200, R"({"bans":[{"discord_id":1}]})"});
	CHECK(g_kicks.empty(), "a detached completion does not kick");
	CHECK(steamhttp::g_requests.size() == 1, "a detached completion sends nothing");

	// OnPluginUpdate after the unload is a no-op.
	g_asBlockDBPlugin.OnPluginUpdate(std::chrono::milliseconds(1000));
	CheckEqInt(g_clientCalls, 0, "a detached update makes no client call");
	CHECK(!g_mapReady);

	// plg reload: a fresh start registers everything exactly once again.
	steamhttp::g_requests.clear();
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	CheckEqInt(g_mapStartRegisters, 2, "a reload registers the map listeners again");
	CheckEqInt(g_mapEndRegisters, 2, "a reload registers the map listeners again");
	CheckEqInt(g_authRegisters, 2, "a reload registers the auth listener again");
	CheckEqInt(g_hookEventCalls, 2, "a reload hooks player_disconnect again");
	CheckEqInt(g_punishRegisters, 2, "a reload registers OnPlayerPunish again");
	CheckEqInt(g_unpunishRegisters, 2, "a reload registers OnOfflinePlayerUnpunish again");
	CHECK(!g_mapReady, "a reload starts with the gate shut");
	MapStart();
	CHECK(g_mapReady, "the reloaded plugin opens the gate on OnMapStart");

	FinishTest();
}

// --- the module has no command surface --------------------------------------------------------

void TestNoCommandSurface() {
	std::printf("-- no commands / no command permissions\n");

	PrepareTest();
	WriteConfig(kTestConfig);
	CHECK(g_asBlockDBPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	MapStart();
	FireAuth(0, 76561198000000000ULL);
	g_asBlockDBPlugin.OnPluginEnd();

	// The original BlockDB module registered no ConCommand and no command listener; the port
	// therefore has no command permission surface either. The fakes would count any call.
	CheckEqInt(g_addConsoleCommandCalls, 0, "AddConsoleCommand must never be called");
	CheckEqInt(g_addCommandListenerCalls, 0, "AddCommandListener must never be called");

	FinishTest();
}

} // namespace

int main() {
	std::printf("as_blockdb_tests (fake s2sdk/admin_system bindings, fake Steam HTTP transport)\n");

	g_baseDir = "/tmp/as_blockdb_tests_" + std::to_string(static_cast<long long>(::getpid()));
	std::error_code error;
	std::filesystem::remove_all(g_baseDir, error);
	std::filesystem::create_directories(g_baseDir + "/configs/admin_system", error);

	InstallFakes();
	ResetWorld();

	TestKv1Reader();
	TestKv1Getters();
	TestKv1Malformed();
	TestConfigCandidates();
	TestShippedConfigs();
	TestLoadConfigWalk();
	TestStartRegistrations();
	TestMapGate();
	TestMapChangeWindowStart();
	TestClientGuards();
	TestAuthEvent();
	TestPunishUnpunish();
	TestBanQueryResponses();
	TestRequestOrder();
	TestPendingKick();
	TestPendingKickSafetyGuards();
	TestReconcile();
	TestDisconnectEvent();
	TestMissingBindings();
	TestUnloadAndDetached();
	TestNoCommandSurface();

	g_asBlockDBPlugin.OnPluginEnd();
	std::filesystem::remove_all(g_baseDir, error);

	std::printf("as_blockdb_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
