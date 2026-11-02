// as_blockdb — Plugify port of the Pisex admin_system module "BlockDB" ([AS] BlockDB):
// reports bans to the BlockDB service (api.blockdb.ru) and announces returning offenders to a
// Discord webhook.
//
// Behaviour of the original, kept:
//   * OnPlayerPunish (from the admin core) with RT_BAN -> POST /v2/bans
//     {"steamid64": <number>, "reason": <reason>, "duration": <seconds>} with the ApiKey in the
//     Authorization header;
//   * OnOfflinePlayerUnpunish with RT_BAN -> DELETE /v2/bans {"steamid64": "<string>"};
//   * on every authorization of a human player -> GET /v2/bans/<steamid64> ("Bearer <ApiKey>").
//     The returned ban list is counted (number of bans plus number of distinct discord ids); when
//     the distinct count reaches AutoBlockBanCount the player is disconnected with
//     NETWORK_DISCONNECT_KICKBANADDED, otherwise a report with one embed per offence is posted
//     to the "webhook" (only when the player has at least one offence). A 60 second per-steamid
//     window suppresses duplicate authorization notifications, like g_mLastConnectTime did.
//
// Pure s2sdk + the admin_system consumer bindings (docs/PLUGIN_STANDARD.md §7): no pisexcompat, no
// s2guard, no hudmenu, no SQL database. The original BlockDB module never had a database of its
// own - all of its state is the remote BlockDB API plus the process-local 60 s dedupe map - so
// there is nothing for plgdb to open and no table/SQL compatibility to preserve. The guards the
// other admin modules got from pisexcompat live in this file:
//   * PITFALLS B1/B2 — the map gate g_mapReady. s2sdk client functions dereference null before the
//     first map and, worse, an emptied client list between OnMapEnd and the next OnMapStart. The
//     map listeners are registered in OnPluginStart; readiness is latched by OnMapStart and by the
//     first OnPluginUpdate of a plugin loaded into a running map (unless an OnMapEnd was seen
//     since OnPluginStart), and dropped by OnMapEnd. No client query is made while it is closed
//     (not even from the authorization listener or the reconcile).
//   * PITFALLS C2 — every client query goes through CanQueryClient/HasClient/HasController: slot
//     0..63, IsClientConnected && !IsClientSourceTV, and a controller entity for the name. The
//     SteamID64 only needs the client object (the original's GetSteamID64 answered for a client
//     that was still connecting); s2sdk's "<unknown>" name becomes "".
//   * PITFALLS C12 — "player_disconnect" is hooked with s2sdk::HookEvent(HookMode::Pre) in
//     OnPluginStart and unhooked in OnPluginEnd. The handler resolves the player-typed "userid"
//     field with GetEventPlayerSlot (a raw user id is not a slot in CS2), treats a negative result
//     as "no player" and answers Continue on every path (Handled would swallow the event for the
//     engine and every other plugin). It only clears this plugin's per-slot state.
//   * a ban-list answer that wants a kick is a bounded pending decision when the map gate is
//     closed or the client is not readable yet (the OnMapEnd -> OnMapStart window, a connecting
//     client). It is retried from OnPluginUpdate for up to 60 s. Both IsFakeClient and
//     IsClientAuthorized bindings are required: a missing binding or an unauthorized client
//     keeps the decision until expiry, a fake client is cleared without a kick, and only then
//     the slot and canonical SteamID64 are re-checked, so the same player is kicked once the
//     map/client is ready while a recycled slot never gets the newcomer's kick; disconnect,
//     reuse, expiry and OnPluginEnd clear it. The retry never sends another GET or webhook.
//   * PITFALLS A1/A7 — OnPluginStart never returns Failed (that stops every Plugify plugin);
//     OnPluginEnd removes the authorization listener, the event hook, the map listeners and the
//     two admin core callbacks, and drops every pending HTTP completion, so `plg reload` leaves
//     nothing that points into the unloaded library. g_active makes a stale callback (the engine
//     may still hold a pointer for a moment) a no-op.
//   * PITFALLS C11 / commands — the module has no console commands of its own (the original had
//     none either): nothing is registered, so there is no command permission surface and nothing
//     to remove. The admin core callbacks are unregistered symmetrically.
//
// Porting notes (the mechanism changed, not the behaviour):
//   * the Metamod plugin hooked IServerGameDLL::GameServerSteamAPIActivated, spun up a
//     detached std::thread and blocked it on an ISteamHTTP call result. Plugify/s2sdk have no
//     Steam API access, so src/steam_http.* re-creates ISteamHTTP on the server's own
//     libsteam_api.so (dlopen + the C accessors it exports) and delivers the completion through
//     a call-result object with Valve's CCallbackBase layout - the same mechanism CCallResult<>
//     used. No thread blocks: requests are asynchronous, completions are handled on the game
//     thread from the plugin's Poll().
//   * the ban query runs against the slot; if the slot changed hands while the request was in
//     flight the kick is skipped (the original could kick the newcomer of a recycled slot). A
//     kick that cannot execute yet is stored and retried, never re-queried.
//   * because the JSON of the BlockDB API is not trusted, every field is type-checked before it
//     is read (the original relied on exceptions caught further up).
//   * s2sdk's OnClientAuthenticated is not delivered reliably (see plugins/perm_connector), so
//     the check also runs from a once-per-second reconcile keyed by the slot's steamid; its first
//     pass only records who is already online, because the original never re-checked players that
//     were already connected.
//   * BlockDB.ini is read by the self-contained KV1 reader in src/config.hpp with the exact
//     pisexcompat ResolvePath order for the unchanged original path: raw cwd
//     addons/configs/admin_system/BlockDB.ini first, then configs/admin_system[/translations] and
//     data/admin_system. The first existing file is authoritative; no new location is invented.
//   * src/steam_http.cpp logs only the origin of a URL on transport failure (SafeUrlForLog), never
//     its path/query - the Discord webhook token lives in the path.

#include "config.hpp"
#include "steam_http.hpp"

#include <admin_api.hpp>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr char kLogTag[] = "BlockDB";
constexpr int32_t kMaxSlots = 64;
constexpr char kBanApiUrl[] = "https://api.blockdb.ru/v2/bans";
// Duplicate-suppression window of the original OnClientAuthorized handler.
constexpr int64_t kConnectDedupeSeconds = 60;
constexpr auto kReconcileInterval = std::chrono::seconds(1);

// The object the ported module uses (the Pisex variable, now the consumer wrapper). The wrapper
// checks the generated binding slots itself, so a missing admin_system turns every call into a
// no-op instead of a null pointer call ("OBJECT bindings": the generated global that carries the
// API object is nullable until the plugify module fills it).
admin_system::AdminApi* g_pAdminApi = nullptr;

// --- config / remote state ---------------------------------------------------------------------

as_blockdb::Config g_config;

// steamid64 -> unix time of the last handled authorization (the original g_mLastConnectTime).
std::map<uint64_t, int64_t> g_lastConnectTime;

// --- PITFALLS B1/B2: the map gate ---------------------------------------------------------------

bool g_active = false;              // the plugin is between OnPluginStart and OnPluginEnd
bool g_mapReady = false;            // a map is up: s2sdk client/entity calls are safe
bool g_mapEndedSinceStart = false;  // OnMapEnd was seen since OnPluginStart
bool g_firstUpdateDone = false;     // the mid-map latch has already been decided
bool g_mapListeners = false;        // OnMapStart/OnMapEnd are registered with s2sdk

// --- registrations the unload has to take back (PITFALLS A7) -----------------------------------

bool g_authListener = false;      // OnClientAuthenticated is registered with s2sdk
bool g_disconnectHooked = false;  // the "player_disconnect" Pre hook is attached

// --- the original's per-slot dedupe state ------------------------------------------------------

std::array<uint64_t, static_cast<std::size_t>(kMaxSlots)> g_seenSteamIds{};
bool g_seenSeeded = false;  // the reconcile already recorded who was online when it started
std::chrono::milliseconds g_sinceReconcile{0};

// A ban-list answer that wants a kick but could not be executed yet (the map gate is closed in
// the OnMapEnd -> OnMapStart window, the client is still connecting, or the slot is momentarily
// unreadable). Retried from OnPluginUpdate without another GET/webhook; cleared on disconnect,
// slot reuse, expiry and unload.
constexpr auto kPendingKickLifetime = std::chrono::seconds(60);
struct PendingKick {
	bool active = false;
	uint64_t steamid = 0;
	std::chrono::steady_clock::time_point expiry{};
};
std::array<PendingKick, static_cast<std::size_t>(kMaxSlots)> g_pendingKicks{};

// --- logging -----------------------------------------------------------------------------------

void Log(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(std::string("[") + kLogTag + "] " + std::string(message), severity,
				 plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	Log(message, plg::Severity::Error);
}

void LogInfo(std::string_view message) {
	Log(message, plg::Severity::Info);
}

// --- small helpers -----------------------------------------------------------------------------

std::string ToString(const plg::string& text) {
	return std::string(text.data(), text.size());
}

std::string FormatTime(int64_t iTime) {
	const std::time_t t = static_cast<std::time_t>(iTime);
	std::tm tm{};
	if (localtime_r(&t, &tm) == nullptr) {
		return "?";
	}
	char buffer[64];
	if (std::strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M:%S", &tm) == 0) {
		return "?";
	}
	return buffer;
}

// Replaces the std::wstring_convert check of the original (the codecvt facet is deprecated and
// would fail this build with -Werror).
bool IsValidUTF8(const std::string& text) {
	std::size_t i = 0;
	while (i < text.size()) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		std::size_t extra = 0;
		uint32_t code = 0;
		if (c < 0x80) {
			++i;
			continue;
		}
		if ((c & 0xE0) == 0xC0) {
			extra = 1;
			code = c & 0x1Fu;
		} else if ((c & 0xF0) == 0xE0) {
			extra = 2;
			code = c & 0x0Fu;
		} else if ((c & 0xF8) == 0xF0) {
			extra = 3;
			code = c & 0x07u;
		} else {
			return false;
		}
		if (i + extra >= text.size()) {
			return false;
		}
		for (std::size_t j = 1; j <= extra; ++j) {
			const unsigned char cc = static_cast<unsigned char>(text[i + j]);
			if ((cc & 0xC0) != 0x80) {
				return false;
			}
			code = (code << 6) | (cc & 0x3Fu);
		}
		// Reject overlong encodings and out-of-range code points.
		if ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) || (extra == 3 && code < 0x10000) ||
			code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
			return false;
		}
		i += extra + 1;
	}
	return true;
}

// The original CleanInvalidUTF8: keep ASCII and lead bytes, drop stray continuation bytes.
std::string CleanInvalidUTF8(const std::string& text) {
	std::string result;
	result.reserve(text.size());
	for (const char c : text) {
		const unsigned char byte = static_cast<unsigned char>(c);
		if ((byte & 0x80) == 0 || (byte & 0xC0) == 0xC0) {
			result += c;
		}
	}
	return result;
}

std::string SanitizeUtf8(const std::string& text) {
	return IsValidUTF8(text) ? text : CleanInvalidUTF8(text);
}

// The original GetCounts: total bans plus the number of distinct discord ids.
std::pair<int, int> GetCounts(const json& bans) {
	const int bansCount = static_cast<int>(bans.size());
	std::set<uint64_t> uniqueDiscordIds;
	for (const auto& ban : bans) {
		if (!ban.is_object() || !ban.contains("discord_id") || ban["discord_id"].is_null()) {
			continue;
		}
		try {
			if (ban["discord_id"].is_number()) {
				uniqueDiscordIds.insert(ban["discord_id"].get<uint64_t>());
			}
		} catch (const json::exception&) {
			// skip values that are not representable
		}
	}
	return {bansCount, static_cast<int>(uniqueDiscordIds.size())};
}

// --- PITFALLS C2: the client guards -------------------------------------------------------------

// The map gate and the slot range are the only requirements for a function that addresses a
// client without reading its state.
bool CanQueryClient(int32_t slot) {
	return g_active && g_mapReady && slot >= 0 && slot < kMaxSlots;
}

// A connected slot with a client object that is not the SourceTV proxy. This is exactly
// pisexcompat's ClientExists() - the guard of the original's GetSteamID64 - and it deliberately
// does NOT require IsClientInGame: the original answered for every connected slot and the ban
// query starts from the authorization path, before the first spawn.
bool HasClient(int32_t slot) {
	if (!CanQueryClient(slot) || !__s2sdk_IsClientConnected || !__s2sdk_IsClientSourceTV) {
		return false;
	}
	return s2sdk::IsClientConnected(slot) && !s2sdk::IsClientSourceTV(slot);
}

// ... and the slot has a controller entity, i.e. a name to read: pisexcompat's ControllerExists(),
// the guard of the original's GetPlayerName. A client that is still connecting has no controller
// yet; the original stored an empty name for it, and so does the port.
bool HasController(int32_t slot) {
	if (!HasClient(slot) || !__s2sdk_PlayerSlotToEntPointer) {
		return false;
	}
	// The pointer is only tested for null; a negative/invalid handle is never dereferenced.
	return s2sdk::PlayerSlotToEntPointer(slot) != nullptr;
}

// The SteamID64 of the original's GetSteamID64(): read for every connected slot (in game or not),
// 0 for the SourceTV proxy, an empty slot or the window between two maps, where the id must not be
// read at all.
uint64_t ClientSteamID64(int32_t slot) {
	if (!HasClient(slot) || !__s2sdk_GetClientSteamID64) {
		return 0;
	}
	return s2sdk::GetClientSteamID64(slot);
}

// The player name of the original's GetPlayerName(): read only with a controller entity, "" for
// s2sdk's "<unknown>" placeholder.
std::string ClientName(int32_t slot) {
	if (!HasController(slot) || !__s2sdk_GetClientName) {
		return {};
	}
	const plg::string name = s2sdk::GetClientName(slot);
	std::string out(name.data(), name.size());
	if (out == "<unknown>") {
		out.clear(); // s2sdk's placeholder for a client without a name
	}
	return out;
}

// --- deferred kick decisions ------------------------------------------------------------------

void ClearPendingKick(int32_t iSlot) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	g_pendingKicks[static_cast<std::size_t>(iSlot)] = PendingKick{};
}

void ClearAllPendingKicks() {
	g_pendingKicks.fill(PendingKick{});
}

// Tries to execute a stored decision. Nothing here touches s2sdk while the map gate is closed:
// the pending entry simply survives until the next update (or its expiry).
bool ResolvePendingKick(int32_t iSlot, std::chrono::steady_clock::time_point now) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return false;
	}
	PendingKick& pending = g_pendingKicks[static_cast<std::size_t>(iSlot)];
	if (!pending.active) {
		return false;
	}
	if (now >= pending.expiry) {
		pending = PendingKick{};
		return false;
	}
	if (!g_active || !g_mapReady) {
		return false; // keep it for after OnMapStart
	}
	if (!HasClient(iSlot)) {
		// The player is disconnected or still connecting: not a final answer yet, keep the
		// decision until the short expiry instead of losing the kick on a map transition.
		return false;
	}
	// Both bindings are required for a safe kick. Without them this slot could be a bot (whose
	// SteamID is normally 0, but it must not be trusted) or a client whose unauthenticated SteamID
	// is already non-zero; keep the decision until the expiry instead of guessing/kicking.
	if (!__s2sdk_IsFakeClient || !__s2sdk_IsClientAuthorized) {
		return false; // missing binding: hold until expiry
	}
	if (s2sdk::IsFakeClient(iSlot)) {
		pending = PendingKick{}; // the slot was reused by a bot - never kick it
		return false;
	}
	if (!s2sdk::IsClientAuthorized(iSlot)) {
		return false; // connected but not authorized yet: keep waiting
	}
	const uint64_t canonical = ClientSteamID64(iSlot);
	if (canonical == 0) {
		return false; // authorized client without a canonical id yet: keep waiting
	}
	if (canonical != pending.steamid) {
		pending = PendingKick{}; // slot reuse: never kick the newcomer
		return false;
	}
	if (!__s2sdk_KickClient) {
		pending = PendingKick{}; // the binding cannot appear later; the decision is unexecutable
		return false;
	}
	s2sdk::KickClient(iSlot, s2sdk::NetworkDisconnectionReason::Kickbanadded, plg::string());
	pending = PendingKick{};
	return true;
}

void QueuePendingKick(int32_t iSlot, uint64_t iSteamID64) {
	if (!g_active || iSlot < 0 || iSlot >= kMaxSlots || iSteamID64 == 0) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	PendingKick& pending = g_pendingKicks[static_cast<std::size_t>(iSlot)];
	pending.active = true;
	pending.steamid = iSteamID64;
	pending.expiry = now + kPendingKickLifetime;
	ResolvePendingKick(iSlot, now); // the common case: the map and the client are ready
}

void TryResolvePendingKicks() {
	const auto now = std::chrono::steady_clock::now();
	for (int32_t iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
		if (g_pendingKicks[static_cast<std::size_t>(iSlot)].active) {
			ResolvePendingKick(iSlot, now);
		}
	}
}

// --- the BlockDB API ---------------------------------------------------------------------------

void PostDiscordReport(const std::string& playerName, uint64_t iSteamID64, const json& bans, int banCount);

// OnGetPlayerBans() of the original, minus the thread/call-result plumbing: everything here runs
// from steamhttp::Poll() on the game thread.
void HandleBanQueryResponse(const steamhttp::Response& response, int32_t iSlot, uint64_t iSteamID64,
							const std::string& playerName) {
	if (!g_active) {
		return; // a detached completion after OnPluginEnd must not touch anything
	}
	const std::string steamid = std::to_string(iSteamID64);
	if (!response.transportOk) {
		LogError("GET " + steamid + " failed (no response - offline server or Steam HTTP unavailable)");
		return;
	}
	if (response.statusCode < 200 || response.statusCode > 299) {
		LogError("GET " + steamid + " failed with status code " + std::to_string(response.statusCode));
		return;
	}
	const json parsed = json::parse(response.body, nullptr, false);
	if (parsed.is_discarded()) {
		LogError("Failed to parse the JSON of the BlockDB response for " + steamid);
		return;
	}
	if (!parsed.is_object() || !parsed.contains("bans") || !parsed["bans"].is_array() ||
		parsed["bans"].empty()) {
		return; // no offences: nothing to report, nobody to kick
	}
	const json& bans = parsed["bans"];
	const auto counts = GetCounts(bans);
	// Exactly the original condition (AutoBlockBanCount 0 therefore kicks everybody who has any
	// offence record - the ini comment "0 = disabled" was never implemented by the Metamod module
	// either).
	if (counts.second >= g_config.autoBlockBanCount) {
		// The decision is stored and retried after the map gate opens / the client becomes ready.
		// The slot and the canonical SteamID are re-checked then, so a recycled slot is never
		// kicked; no second GET and no webhook are sent for it.
		QueuePendingKick(iSlot, iSteamID64);
		return;
	}
	if (counts.first == 0) {
		return;
	}
	PostDiscordReport(playerName, iSteamID64, bans, counts.first);
}

void QueryBansAndReport(int32_t iSlot, uint64_t iSteamID64, const std::string& playerName) {
	if (!g_active) {
		return;
	}
	const std::string url = std::string(kBanApiUrl) + "/" + std::to_string(iSteamID64);
	// The GET is the only call of the original that used a "Bearer " prefix.
	steamhttp::SendRequest(steamhttp::Method::Get, url, "Bearer " + g_config.apiKey, "application/json", "",
						   [iSlot, iSteamID64, playerName](const steamhttp::Response& response) {
							   HandleBanQueryResponse(response, iSlot, iSteamID64, playerName);
						   });
}

void PostDiscordReport(const std::string& playerName, uint64_t iSteamID64, const json& bans, int banCount) {
	if (!g_active || g_config.webhook.empty()) {
		return;
	}
	const std::string steamid = std::to_string(iSteamID64);

	// The original built this list in a 1024 byte char buffer with strcat(); the port stops
	// adding entries at that size instead of overflowing (one Discord embed field holds 1024
	// characters).
	constexpr std::size_t kMaxBansText = 1024;
	std::string bansText;
	for (const auto& ban : bans) {
		if (bansText.size() >= kMaxBansText) {
			break;
		}
		std::string reason = "undefined";
		std::string projectName = "undefined";
		if (ban.is_object()) {
			if (ban.contains("reason") && ban["reason"].is_string()) {
				reason = SanitizeUtf8(ban["reason"].get<std::string>());
			}
			if (ban.contains("project_name") && ban["project_name"].is_string()) {
				projectName = SanitizeUtf8(ban["project_name"].get<std::string>());
			}
		}
		int64_t createdAt = 0;
		if (ban.is_object() && ban.contains("created_at") && ban["created_at"].is_number()) {
			createdAt = ban["created_at"].get<int64_t>();
		}
		bansText += "```- " + reason + " | " + projectName + " | " + FormatTime(createdAt) + "```\n";
	}

	const json report = {
		{"content", nullptr},
		{"embeds",
		 {{
			 {"title", "Новое подключение нарушителя"},
			 {"fields",
			  {{{"name", "Steam Профиль"},
				{"value", "[" + playerName + "](https://steamcommunity.com/profiles/" + steamid + ")"},
				{"inline", true}},
			   {{"name", "Игрок (SteamID64)"}, {"value", "```" + steamid + "```"}, {"inline", true}},
			   {{"name", "Список предыдущих нарушений (" + std::to_string(banCount) + ")"}, {"value", bansText}}}},
			 {"footer", {{"text", g_config.serverName + " | " + FormatTime(static_cast<int64_t>(std::time(nullptr)))}}}}}},
		{"attachments", json::array()},
	};
	// error_handler_t::replace: a player name with broken encoding must not make dump() throw.
	steamhttp::SendRequest(steamhttp::Method::Post, g_config.webhook, "", "application/json",
						   report.dump(4, ' ', false, json::error_handler_t::replace), {});
}

// The original PlayerPunishCallback: a ban is reported with the ApiKey in the Authorization header.
void HandlePlayerPunish(int32_t iSlot, int32_t iType, int32_t iTime, const char* szReason) {
	if (!g_active || iType != admin_system::RT_BAN) {
		return;
	}
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return; // a negative/invalid admin slot never reaches a client query
	}
	const uint64_t steamid = ClientSteamID64(iSlot);
	if (steamid == 0) {
		return;
	}
	try {
		const json body = {
			{"steamid64", steamid},
			{"reason", szReason ? szReason : ""},
			{"duration", iTime},
		};
		steamhttp::SendRequest(steamhttp::Method::Post, kBanApiUrl, g_config.apiKey, "application/json", body.dump(), {});
	} catch (const std::exception& ex) {
		LogError(std::string("could not report the ban: ") + ex.what());
	}
}

// The original OfflinePlayerUnpunishCallback: a DELETE to the same API carrying the steamid.
void HandleOfflinePlayerUnpunish(const char* szSteamID64, int32_t iType) {
	if (!g_active || iType != admin_system::RT_BAN || szSteamID64 == nullptr || *szSteamID64 == '\0') {
		return;
	}
	try {
		const json body = {{"steamid64", szSteamID64}};
		steamhttp::SendRequest(steamhttp::Method::Delete, kBanApiUrl, g_config.apiKey, "application/json", body.dump(), {});
	} catch (const std::exception& ex) {
		LogError(std::string("could not report the unban: ") + ex.what());
	}
}

// The ported OnClientAuthorized handler.
void HandleClientAuthorized(int32_t iSlot, uint64_t iSteamID64) {
	if (!g_active || iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	// The s2sdk event may report a 32-bit account id or 0 (see plugins/perm_connector); the ban
	// query must use the slot's own SteamID64. The lookup is a client call, so it only happens
	// while the map gate is open - outside of it ClientSteamID64() answers 0 without touching
	// s2sdk, and a full SteamID64 from the event is still usable (HTTP only, no client call).
	const uint64_t canonical = ClientSteamID64(iSlot);
	if (canonical != 0) {
		iSteamID64 = canonical;
	} else if (iSteamID64 < (1ull << 32)) {
		return; // not a SteamID64 yet - the reconcile pass picks the player up
	}
	const auto index = static_cast<std::size_t>(iSlot);
	if (iSteamID64 == 0) {
		return;
	}
	if (g_pendingKicks[index].active && g_pendingKicks[index].steamid != iSteamID64) {
		g_pendingKicks[index] = PendingKick{}; // the slot now belongs to another player
	}
	if (g_seenSteamIds[index] == iSteamID64) {
		return; // already handled by the event or a previous reconcile pass
	}
	g_seenSteamIds[index] = iSteamID64;

	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	// Drop entries that can no longer suppress anything: the original's map grew one entry per
	// unique steamid for the process lifetime (audit P3), the dedupe window is the only use.
	for (auto it = g_lastConnectTime.begin(); it != g_lastConnectTime.end();) {
		if (now - it->second >= kConnectDedupeSeconds) {
			it = g_lastConnectTime.erase(it);
		} else {
			++it;
		}
	}
	const auto known = g_lastConnectTime.find(iSteamID64);
	if (known != g_lastConnectTime.end() && now - known->second < kConnectDedupeSeconds) {
		return;
	}
	g_lastConnectTime[iSteamID64] = now;

	QueryBansAndReport(iSlot, iSteamID64, ClientName(iSlot));
}

// Once-per-second replacement for the (unreliable) s2sdk OnClientAuthenticated event.
void ReconcileAuthorized() {
	if (!g_active || !g_mapReady) {
		// The map-end handler already cleared the per-slot state; while the gate is closed no
		// client query (not even IsClientConnected) may run.
		return;
	}
	for (int32_t iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
		const auto index = static_cast<std::size_t>(iSlot);
		if (!HasClient(iSlot)) {
			g_seenSteamIds[index] = 0;
			continue;
		}
		if (!__s2sdk_IsFakeClient || !__s2sdk_IsClientAuthorized) {
			continue;
		}
		if (s2sdk::IsFakeClient(iSlot) || !s2sdk::IsClientAuthorized(iSlot)) {
			continue;
		}
		const uint64_t steamid = ClientSteamID64(iSlot);
		if (steamid == 0 || steamid == g_seenSteamIds[index]) {
			continue;
		}
		if (!g_seenSeeded) {
			g_seenSteamIds[index] = steamid;
			continue;
		}
		HandleClientAuthorized(iSlot, steamid);
	}
	g_seenSeeded = true;
}

// --- the hooked player-typed game event (PITFALLS C12) -----------------------------------------

s2sdk::ResultType OnPlayerDisconnectEvent(const plg::string& /*name*/, void* event,
										  bool /*dontBroadcast*/) {
	try {
		if (!g_active || event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		// "userid" is a player_controller field, so it has to be translated to a slot; a negative
		// answer (invalid or missing player) must never index the per-slot arrays.
		const int32_t slot = s2sdk::GetEventPlayerSlot(event, "userid");
		if (slot >= 0 && slot < kMaxSlots) {
			g_seenSteamIds[static_cast<std::size_t>(slot)] = 0;
			ClearPendingKick(slot);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("player_disconnect handler threw: ") + ex.what());
	} catch (...) {
		LogError("player_disconnect handler threw an unknown exception");
	}
	// Continue: the engine's own disconnect handling (and every other plugin's hook) must run.
	return s2sdk::ResultType::Continue;
}

// --- the s2sdk listeners (PITFALLS B1/B2) -------------------------------------------------------

void OnMapStartListener() {
	try {
		if (!g_active) {
			return; // a stale callback after OnPluginEnd must not open anything
		}
		g_mapReady = true;
	} catch (...) {
	}
}

void OnMapEndListener() {
	try {
		if (!g_active) {
			return;
		}
		// From here until the next map start every s2sdk client query can crash (PITFALLS B2).
		// State only: no client call.
		g_mapReady = false;
		g_mapEndedSinceStart = true;
		g_seenSteamIds.fill(0);
	} catch (...) {
	}
}

void OnClientAuthenticatedListener(int32_t iSlot, uint64_t iSteamID64) {
	try {
		if (!g_active) {
			return;
		}
		HandleClientAuthorized(iSlot, iSteamID64);
	} catch (const std::exception& ex) {
		LogError(std::string("OnClientAuthenticated handler threw: ") + ex.what());
	} catch (...) {
		LogError("OnClientAuthenticated handler threw an unknown exception");
	}
}

// --- config ------------------------------------------------------------------------------------

// LoadConfig() of the original, now through the self-contained KV1 reader. The candidate order is
// pisexcompat's ResolvePath order for the unchanged Metamod path
// "addons/configs/admin_system/BlockDB.ini" (raw cwd file, then configs/admin_system,
// configs/admin_system/translations, data/admin_system). The first *existing* file is
// authoritative: a malformed raw file keeps the built-in defaults rather than silently falling
// through to another file. A missing file keeps the defaults and logs, exactly like the original's
// failed LoadFromFile did. Only the path is logged, never a key or a webhook/key value.
void LoadConfig() {
	g_config = as_blockdb::Config{};

	std::string configsDir;
	if (plg::GetConfigsDir) {
		configsDir = ToString(plg::GetConfigsDir());
	}
	std::string dataDir;
	if (plg::GetDataDir) {
		dataDir = ToString(plg::GetDataDir());
	}

	const std::vector<std::string> candidates = as_blockdb::ConfigCandidates(configsDir, dataDir);
	std::string lookedAt;
	for (const std::string& path : candidates) {
		if (!lookedAt.empty()) {
			lookedAt += ", ";
		}
		lookedAt += path;
		if (!as_blockdb::FileExists(path)) {
			continue;
		}
		if (as_blockdb::LoadConfigFromFile(path, g_config)) {
			LogInfo("loaded " + path);
		} else {
			// g_pUtils->ErrorLog("[%s] Failed to load %s", GetLogTag(), pszPath) of the original:
			// the first existing file is the one the original tried, so there is no fallback.
			LogError("Failed to load " + path);
		}
		return;
	}

	LogError("BlockDB.ini not found (looked at " + lookedAt + ") - using the built-in defaults");
}

// --- lifecycle ---------------------------------------------------------------------------------

class AsBlockDBPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			g_pAdminApi = &admin_system::AdminApi::Get();
			// A reload (plg reload) starts from a clean sheet; OnPluginEnd already unregistered
			// everything, so the flags are simply re-armed here (PITFALLS A7).
			g_active = true;
			g_mapReady = false;
			g_mapEndedSinceStart = false;
			g_firstUpdateDone = false;
			g_seenSteamIds.fill(0);
			ClearAllPendingKicks();
			g_lastConnectTime.clear();
			g_seenSeeded = false;
			g_sinceReconcile = kReconcileInterval;

			// File I/O only: no client, entity or schema call happens in OnPluginStart
			// (PITFALLS B1).
			LoadConfig();

			if (g_config.apiKey.empty()) {
				LogError("No ApiKey configured in BlockDB.ini - the BlockDB API calls will be rejected");
			}
			// Best effort: a server without libsteam_api.so (or a future SDK) simply keeps the
			// features that do not need HTTP.
			steamhttp::Init();

			// PITFALLS B1: registering the map listeners touches no client. Without them nothing
			// could ever close the gate again, so a missing binding keeps it shut (CanQueryClient
			// stays false) instead of opening it blindly.
			if (!g_mapListeners) {
				if (__s2sdk_OnMapStart_Register && __s2sdk_OnMapEnd_Register) {
					s2sdk::OnMapStart_Register(&OnMapStartListener);
					s2sdk::OnMapEnd_Register(&OnMapEndListener);
					g_mapListeners = true;
				} else {
					LogError("the s2sdk map listeners are unavailable - client calls stay disabled");
				}
			}

			// The direct equivalent of the original's HookOnClientAuthorized. It is not reliable
			// on this stack (PITFALLS B6), hence the reconcile pass; removing it in OnPluginEnd is
			// still mandatory (PITFALLS A7).
			if (!g_authListener) {
				if (__s2sdk_OnClientAuthenticated_Register && __s2sdk_OnClientAuthenticated_Unregister) {
					s2sdk::OnClientAuthenticated_Register(&OnClientAuthenticatedListener);
					g_authListener = true;
				} else {
					LogError("s2sdk OnClientAuthenticated is unavailable - the once-per-second reconcile "
							 "is the only authorization signal");
				}
			}

			// PITFALLS C12: one Pre hook on "player_disconnect"; the handler answers Continue on
			// every path, so the engine's own disconnect handling is untouched.
			if (!g_disconnectHooked) {
				if (__s2sdk_HookEvent && __s2sdk_UnhookEvent) {
					if (s2sdk::HookEvent("player_disconnect", &OnPlayerDisconnectEvent,
										 s2sdk::HookMode::Pre) == s2sdk::EventHookError::Okay) {
						g_disconnectHooked = true;
					} else {
						LogError("could not hook \"player_disconnect\" - the per-slot state is only "
								 "cleared by the reconcile");
					}
				} else {
					LogError("the s2sdk event bindings are unavailable - \"player_disconnect\" is not hooked");
				}
			}

			// The two callbacks of the admin core. The consumer wrapper stores them even when the
			// generated admin_system bindings are not filled yet and makes every call a no-op, so
			// nothing here can dereference null.
			if (g_pAdminApi != nullptr) {
				g_pAdminApi->OnPlayerPunish([](int iSlot, int iType, int iTime, const char* szReason, int iAdminID) {
					(void)iAdminID;
					HandlePlayerPunish(iSlot, iType, iTime, szReason);
				});
				g_pAdminApi->OnOfflinePlayerUnpunish([](const char* szSteamID64, int iType, int iAdminID) {
					(void)iAdminID;
					HandleOfflinePlayerUnpunish(szSteamID64, iType);
				});
			}

			LogInfo("ready (server_name " + (g_config.serverName.empty() ? std::string("<empty>") : g_config.serverName) +
					", AutoBlockBanCount " + std::to_string(g_config.autoBlockBanCount) + ")");
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginStart failed: ") + ex.what());
		} catch (...) {
			LogError("OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed (PITFALLS A1: that stops every plugify plugin)
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override {
		try {
			if (!g_active) {
				return {};
			}
			// A plugin loaded into a running map never saw OnMapStart: its first update is the
			// "a map is up" signal (PITFALLS B1). An OnMapEnd seen since OnPluginStart means the
			// server is between maps, so the latch waits for the next OnMapStart instead.
			if (!g_firstUpdateDone) {
				g_firstUpdateDone = true;
				if (g_mapListeners && !g_mapEndedSinceStart) {
					g_mapReady = true;
				}
			}
			steamhttp::Poll(); // runs the finished HTTP handlers on the game thread
			TryResolvePendingKicks(); // after OnMapStart / once the client is readable again
			g_sinceReconcile += dt;
			if (g_sinceReconcile >= kReconcileInterval) {
				g_sinceReconcile = std::chrono::milliseconds(0);
				ReconcileAuthorized();
			}
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginUpdate failed: ") + ex.what());
		} catch (...) {
			LogError("OnPluginUpdate failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			// From this point a callback that the engine or a pending HTTP completion still holds
			// must be a no-op (PITFALLS A7).
			g_active = false;
			g_mapReady = false;

			if (g_authListener) {
				if (__s2sdk_OnClientAuthenticated_Unregister) {
					s2sdk::OnClientAuthenticated_Unregister(&OnClientAuthenticatedListener);
				}
				g_authListener = false;
			}

			if (g_disconnectHooked) {
				if (__s2sdk_UnhookEvent) {
					s2sdk::UnhookEvent("player_disconnect", &OnPlayerDisconnectEvent, s2sdk::HookMode::Pre);
				}
				g_disconnectHooked = false;
			}

			if (g_mapListeners) {
				if (__s2sdk_OnMapStart_Unregister) {
					s2sdk::OnMapStart_Unregister(&OnMapStartListener);
				}
				if (__s2sdk_OnMapEnd_Unregister) {
					s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
				}
				g_mapListeners = false;
			}

			// The admin core callbacks point into this library as well. Unregister also clears the
			// wrapper's std::function, so a stale trampoline call is harmless.
			if (g_pAdminApi != nullptr) {
				g_pAdminApi->OnPlayerPunishUnregister();
				g_pAdminApi->OnOfflinePlayerUnpunishUnregister();
			}
			g_pAdminApi = nullptr;

			// Drop every pending completion: their handlers call into this library. Poll() first
			// delivers the responses that already arrived (g_active == false makes them harmless).
			steamhttp::Shutdown();

			g_seenSteamIds.fill(0);
			ClearAllPendingKicks();
			g_lastConnectTime.clear();
			g_seenSeeded = false;
			g_sinceReconcile = std::chrono::milliseconds(0);
			g_mapEndedSinceStart = true;
			g_firstUpdateDone = false;
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginEnd failed: ") + ex.what());
		} catch (...) {
			LogError("OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

AsBlockDBPlugin g_asBlockDBPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_asBlockDBPlugin)
