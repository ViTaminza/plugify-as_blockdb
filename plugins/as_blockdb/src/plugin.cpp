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
// Porting notes (the mechanism changed, not the behaviour):
//   * the Metamod plugin hooked IServerGameDLL::GameServerSteamAPIActivated, spun up a
//     detached std::thread and blocked it on an ISteamHTTP call result. Plugify/s2sdk have no
//     Steam API access, so src/steam_http.* re-creates ISteamHTTP on the server's own
//     libsteam_api.so (dlopen + the C accessors it exports) and delivers the completion through
//     a call-result object with Valve's CCallbackBase layout - the same mechanism CCallResult<>
//     used. No thread blocks: requests are asynchronous, completions are handled on the game
//     thread from the plugin's Poll().
//   * the ban query runs against the slot; if the slot changed hands while the request was in
//     flight the kick is skipped (the original could kick the newcomer of a recycled slot).
//   * because the JSON of the BlockDB API is not trusted, every field is type-checked before it
//     is read (the original relied on exceptions caught further up).
//   * s2sdk's OnClientAuthenticated is not delivered reliably on a live server (see
//     plugins/perm_connector), so the check also runs from a once-per-second reconcile keyed by
//     the slot's steamid; its first pass only records who is already online, because the original
//     never re-checked players that were already connected.

#include "steam_http.hpp"

#include <admin_api.hpp>

#include <pisex/pisex.hpp>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace {

using json = nlohmann::json;

// The object the ported module uses (the Pisex variable, now the consumer wrapper).
admin_system::AdminApi* g_pAdminApi = &admin_system::AdminApi::Get();

constexpr char kLogTag[] = "BlockDB";
constexpr int kMaxSlots = 64;
constexpr char kConfigPath[] = "addons/configs/admin_system/BlockDB.ini";
constexpr char kBanApiUrl[] = "https://api.blockdb.ru/v2/bans";
// Duplicate-suppression window of the original OnClientAuthorized handler.
constexpr int64_t kConnectDedupeSeconds = 60;

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

class AsBlockDBPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	// --- BlockDB.ini ---------------------------------------------------------------------------
	std::string m_serverName;
	std::string m_apiKey;
	int m_autoBlockBanCount = 0;
	std::string m_webhook;

	// steamid64 -> unix time of the last handled authorization (the original g_mLastConnectTime).
	std::map<uint64_t, int64_t> m_lastConnectTime;

	void LoadConfig();

	// The ported admin core callbacks.
	void OnPlayerPunish(int iSlot, int iType, int iTime, const char* szReason);
	void OnOfflinePlayerUnpunish(const char* szSteamID64, int iType);

	// The ported OnClientAuthorized handler.
	void HandleClientAuthorized(int iSlot, uint64_t iSteamID64);
	void QueryBansAndReport(int iSlot, uint64_t iSteamID64, const std::string& playerName);
	void PostDiscordReport(const std::string& playerName, uint64_t iSteamID64, const json& bans, int banCount);

	// Once-per-second replacement for the (unreliable) s2sdk OnClientAuthenticated event.
	void ReconcileAuthorized();
	static constexpr auto kReconcileInterval = std::chrono::seconds(1);
	std::array<uint64_t, kMaxSlots> m_seenSteamIds{};
	bool m_seenSeeded = false;
	std::chrono::milliseconds m_sinceReconcile{0};
};

AsBlockDBPlugin g_asBlockDBPlugin;

// --- the ported module ------------------------------------------------------------------------

void AsBlockDBPlugin::LoadConfig() {
	pisex::KeyValues::AutoDelete kv("BlockDB");
	if (!kv->LoadFromFile(kConfigPath)) {
		pisex::g_pUtils->ErrorLog("[%s] Failed to load %s", kLogTag, kConfigPath);
		return; // defaults: disabled API key / webhook, AutoBlockBanCount 0
	}
	m_serverName = kv->GetString("server_name");
	m_apiKey = kv->GetString("ApiKey");
	m_autoBlockBanCount = kv->GetInt("AutoBlockBanCount");
	m_webhook = kv->GetString("webhook");
}

void AsBlockDBPlugin::OnPlayerPunish(int iSlot, int iType, int iTime, const char* szReason) {
	if (iType != admin_system::RT_BAN) {
		return;
	}
	const uint64_t steamid = pisex::g_pPlayers->GetSteamID64(iSlot);
	if (steamid == 0) {
		return;
	}
	// The original: {"steamid64": <number>, "reason": <reason>, "duration": <seconds>} with the
	// ApiKey as the Authorization header, POST https://api.blockdb.ru/v2/bans.
	const json body = {
		{"steamid64", steamid},
		{"reason", szReason ? szReason : ""},
		{"duration", iTime},
	};
	steamhttp::SendRequest(steamhttp::Method::Post, kBanApiUrl, m_apiKey, "application/json", body.dump(), {});
}

void AsBlockDBPlugin::OnOfflinePlayerUnpunish(const char* szSteamID64, int iType) {
	if (iType != admin_system::RT_BAN || szSteamID64 == nullptr || *szSteamID64 == '\0') {
		return;
	}
	// The original: {"steamid64": "<string>"}, DELETE https://api.blockdb.ru/v2/bans.
	const json body = {{"steamid64", szSteamID64}};
	steamhttp::SendRequest(steamhttp::Method::Delete, kBanApiUrl, m_apiKey, "application/json", body.dump(), {});
}

void AsBlockDBPlugin::HandleClientAuthorized(int iSlot, uint64_t iSteamID64) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	// The s2sdk event may report a 32-bit account id or 0 (see plugins/perm_connector); the ban
	// query must use the slot's own SteamID64.
	const uint64_t canonical = pisex::g_pPlayers->GetSteamID64(iSlot);
	if (canonical != 0) {
		iSteamID64 = canonical;
	} else if (iSteamID64 < (1ull << 32)) {
		return; // not a SteamID64 yet - the reconcile pass picks the player up
	}
	if (iSteamID64 == 0 || m_seenSteamIds[static_cast<std::size_t>(iSlot)] == iSteamID64) {
		return; // already handled by the event or a previous reconcile pass
	}
	m_seenSteamIds[static_cast<std::size_t>(iSlot)] = iSteamID64;

	const auto known = m_lastConnectTime.find(iSteamID64);
	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	if (known != m_lastConnectTime.end() && now - known->second < kConnectDedupeSeconds) {
		return;
	}
	m_lastConnectTime[iSteamID64] = now;

	QueryBansAndReport(iSlot, iSteamID64, pisex::g_pPlayers->GetPlayerName(iSlot));
}

void AsBlockDBPlugin::QueryBansAndReport(int iSlot, uint64_t iSteamID64, const std::string& playerName) {
	const std::string steamid = std::to_string(iSteamID64);
	const std::string url = std::string(kBanApiUrl) + "/" + steamid;
	// The GET is the only call of the original that used a "Bearer " prefix.
	steamhttp::SendRequest(steamhttp::Method::Get, url, "Bearer " + m_apiKey, "application/json", "",
						   [this, iSlot, iSteamID64, playerName](const steamhttp::Response& response) {
							   if (!response.transportOk) {
								   pisex::g_pUtils->ErrorLog("[%s] GET %s failed (no response - offline server or Steam HTTP unavailable)",
															 kLogTag, std::to_string(iSteamID64).c_str());
								   return;
							   }
							   if (response.statusCode < 200 || response.statusCode > 299) {
								   // The original: "HTTP request failed with status code %i".
								   pisex::g_pUtils->ErrorLog("[%s] GET %s failed with status code %i", kLogTag,
															 std::to_string(iSteamID64).c_str(), response.statusCode);
								   return;
							   }
							   const json parsed = json::parse(response.body, nullptr, false);
							   if (parsed.is_discarded()) {
								   pisex::g_pUtils->ErrorLog("[%s] Failed to parse the JSON of the BlockDB response for %s",
															 kLogTag, std::to_string(iSteamID64).c_str());
								   return;
							   }
							   if (!parsed.is_object() || !parsed.contains("bans") || !parsed["bans"].is_array() ||
								   parsed["bans"].empty()) {
								   return; // no offences: nothing to report, nobody to kick
							   }
							   const json& bans = parsed["bans"];
							   const auto counts = GetCounts(bans);
							   // Exactly the original condition (AutoBlockBanCount 0 therefore kicks
							   // everybody who has any offence record - the ini comment "0 = disabled"
							   // was never implemented by the Metamod module either).
							   if (counts.second >= m_autoBlockBanCount) {
								   // Kick only when the slot still holds the player the ban list was
								   // requested for (a response can arrive after a slot change).
								   if (pisex::g_pPlayers->IsConnected(iSlot) &&
									   pisex::g_pPlayers->GetSteamID64(iSlot) == iSteamID64 && __s2sdk_KickClient) {
									   s2sdk::KickClient(iSlot, s2sdk::NetworkDisconnectionReason::Kickbanadded, plg::string());
								   }
								   return;
							   }
							   if (counts.first == 0) {
								   return;
							   }
							   PostDiscordReport(playerName, iSteamID64, bans, counts.first);
						   });
}

void AsBlockDBPlugin::PostDiscordReport(const std::string& playerName, uint64_t iSteamID64, const json& bans,
										int banCount) {
	if (m_webhook.empty()) {
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
			 {"footer", {{"text", m_serverName + " | " + FormatTime(static_cast<int64_t>(std::time(nullptr)))}}}}}},
		{"attachments", json::array()},
	};
	// error_handler_t::replace: a player name with broken encoding must not make dump() throw
	// (the Metamod module relied on an outer catch it did not always have).
	steamhttp::SendRequest(steamhttp::Method::Post, m_webhook, "", "application/json",
						   report.dump(4, ' ', false, json::error_handler_t::replace), {});
}

void AsBlockDBPlugin::ReconcileAuthorized() {
	for (int iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
		if (!pisex::g_pPlayers->IsConnected(iSlot)) {
			m_seenSteamIds[static_cast<std::size_t>(iSlot)] = 0;
			continue;
		}
		if (pisex::g_pPlayers->IsFakeClient(iSlot) || !pisex::g_pPlayers->IsAuthenticated(iSlot)) {
			continue;
		}
		const uint64_t steamid = pisex::g_pPlayers->GetSteamID64(iSlot);
		if (steamid == 0 || steamid == m_seenSteamIds[static_cast<std::size_t>(iSlot)]) {
			continue;
		}
		if (!m_seenSeeded) {
			m_seenSteamIds[static_cast<std::size_t>(iSlot)] = steamid;
			continue;
		}
		HandleClientAuthorized(iSlot, steamid);
	}
	m_seenSeeded = true;
}

// --- lifecycle ---------------------------------------------------------------------------------

plg::PluginResult AsBlockDBPlugin::OnPluginStart() {
	try {
		// "admin_system" makes pisexcompat resolve "addons/configs/admin_system/..." through the
		// shared <configs>/admin_system directory every admin plugin uses.
		pisex::Init("admin_system");
		LoadConfig();

		if (m_apiKey.empty()) {
			pisex::g_pUtils->ErrorLog("[%s] No ApiKey configured in %s - the BlockDB API calls will be rejected", kLogTag,
									  kConfigPath);
		}
		// Best effort: a server without libsteam_api.so (or a future SDK) simply keeps the
		// features that do not need HTTP.
		steamhttp::Init();

		g_pAdminApi->OnPlayerPunish([](int iSlot, int iType, int iTime, const char* szReason, int iAdminID) {
			(void)iAdminID;
			g_asBlockDBPlugin.OnPlayerPunish(iSlot, iType, iTime, szReason);
		});
		g_pAdminApi->OnOfflinePlayerUnpunish([](const char* szSteamID64, int iType, int iAdminID) {
			(void)iAdminID;
			g_asBlockDBPlugin.OnOfflinePlayerUnpunish(szSteamID64, iType);
		});

		pisex::g_pPlayers->HookOnClientAuthorized([](int iSlot, uint64_t iSteamID64) {
			g_asBlockDBPlugin.HandleClientAuthorized(iSlot, iSteamID64);
		});

		m_sinceReconcile = kReconcileInterval;
		return {};
	} catch (const std::exception& ex) {
		pisex::g_pUtils->ErrorLog("[%s] OnPluginStart failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::g_pUtils->ErrorLog("[%s] OnPluginStart failed with an unknown exception", kLogTag);
	}
	return {};
}

plg::PluginResult AsBlockDBPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		pisex::Poll();
		steamhttp::Poll(); // runs the finished HTTP handlers (on the game thread)
		m_sinceReconcile += dt;
		if (m_sinceReconcile >= kReconcileInterval) {
			m_sinceReconcile = std::chrono::milliseconds(0);
			ReconcileAuthorized();
		}
	} catch (const std::exception& ex) {
		pisex::g_pUtils->ErrorLog("[%s] OnPluginUpdate failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::g_pUtils->ErrorLog("[%s] OnPluginUpdate failed with an unknown exception", kLogTag);
	}
	return {};
}

plg::PluginResult AsBlockDBPlugin::OnPluginEnd() {
	try {
		// Drop the admin core listeners first: they point into this library, which is about to
		// go away.
		g_pAdminApi->OnPlayerPunishUnregister();
		g_pAdminApi->OnOfflinePlayerUnpunishUnregister();
		steamhttp::Shutdown();
		pisex::Shutdown();
	} catch (...) {
		// nothing left to do at unload
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_asBlockDBPlugin)
