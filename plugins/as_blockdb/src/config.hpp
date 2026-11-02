// as_blockdb — the config half of the module: where BlockDB.ini is read from and the
// self-contained Valve KeyValues (KV1) reader that parses it.
//
// Self-contained on purpose (docs/PLUGIN_STANDARD.md §7: no pisexcompat, no helper library). The
// original module loaded the file through hl2sdk's KeyValues and looked the keys up with
// GetString/GetInt; this header keeps the file format and those getter semantics, so the
// production .ini files are read unchanged.
//
// 1. The file. Production keeps the Pisex file, comments included:
//
//        "BlockDB"
//        {
//            "server_name"           "MIRAGE #1" // Название сервера
//            "ApiKey"                ""          // Ключ для доступа к API
//            "AutoBlockBanCount"     "3"         // ...
//            "webhook"               ""          // ...
//        }
//
//    ConfigCandidates() restores the exact pisexcompat ResolvePath() order for the original
//    relative path "addons/configs/admin_system/BlockDB.ini": the raw cwd-relative file first,
//    then <configs>/admin_system/BlockDB.ini, <configs>/admin_system/translations/BlockDB.ini and
//    <data>/admin_system/BlockDB.ini. The first *existing* file is authoritative - a malformed
//    raw file keeps the built-in defaults instead of silently falling through to a decoy - and a
//    missing file keeps the built-in defaults and logs, like the original's failed LoadFromFile
//    did. The caller supplies the configs and data directories (plg::GetConfigsDir() /
//    plg::GetDataDir() in src/plugin.cpp); this header never touches the Plugify API.
//
// 2. ParseKv1()/LoadKv1File(): a read-only Valve KeyValues (KV1) reader. The accepted grammar is
//    Valve's: quoted ("key" "value") and unquoted (key value) tokens, a key may carry a value and a
//    block, nested blocks, // and /* */ comments, \" \\ \n \r \t escapes inside quotes, and
//    #base / #include / #define lines are skipped instead of followed. Like Valve's loader, a file
//    that is a single "BlockDB" { ... } block is returned as *that block's contents*, which is why
//    `GetString("server_name")` reads a key inside it (the original's `KeyValues kv("BlockDB");
//    kv.LoadFromFile(...); kv.GetString("server_name")`).
//    A malformed file - an unterminated string, a missing '}' or a stray top-level '}' - is
//    rejected as a whole (std::nullopt), so half a config never overrides the defaults. A UTF-8 BOM
//    is ignored.
//
// 3. GetInt(): the semantics of pisexcompat's KeyValues, i.e. of the original's getter - the value
//    is trimmed, an empty/missing value returns the caller's default, and a parse failure falls
//    back to atoi ("5 // x" -> 5, "1.5" -> 1, "abc" -> 0).
//
// 4. Config: the four keys of the original with the original's defaults (empty strings, count 0).
//    A missing key keeps the default and a missing/malformed file leaves the caller's values
//    untouched - exactly what the original's `if (hKv->LoadFromFile(...))` guard does.

#pragma once

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace as_blockdb {

// --- 1. the KV1 reader ------------------------------------------------------------------------

struct KvNode {
	std::string name;
	std::string value;
	bool hasValue = false;
	bool isBlock = false;
	std::vector<KvNode> children;

	// First child with that name (case-insensitive, like Valve's FindKey), or nullptr.
	const KvNode* Find(std::string_view key) const;
	// The child's value; `def` when the child is missing or carries no value.
	std::string_view GetString(std::string_view key, std::string_view def = {}) const;
	// The child's value as an int with the original's atoi fallback; `def` when the child is
	// missing or its value is empty.
	int GetInt(std::string_view key, int def = 0) const;
	// The child's boolean ("true"/"yes"/"on" -> true, "false"/"no"/"off" -> false, otherwise the
	// integer test); `def` when the child is missing or its value is empty.
	bool GetBool(std::string_view key, bool def = false) const;
};

namespace detail {

inline char LowerCase(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

inline bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (LowerCase(a[i]) != LowerCase(b[i])) {
			return false;
		}
	}
	return true;
}

inline std::string_view Trim(std::string_view text) {
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
		--end;
	}
	return text.substr(begin, end - begin);
}

// The original's atoi fallback for a non-numeric value ("1.5" -> 1, "abc" -> 0).
inline int Atoi(std::string_view text) {
	const std::string copy(text);
	return std::atoi(copy.c_str());
}

//   file  := node* EOF
//   node  := token [ token ] [ '{' node* '}' ]
//   token := '"' escaped '"' | raw
class Parser {
public:
	explicit Parser(std::string_view text) : text_(text) {}

	bool Parse(KvNode& root) {
		ParseNodes(root, false);
		return !failed_;
	}

private:
	void SkipSpace() {
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c))) {
				++pos_;
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
				pos_ += 2;
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '*') {
				pos_ += 2;
				while (pos_ + 1 < text_.size() && !(text_[pos_] == '*' && text_[pos_ + 1] == '/')) {
					++pos_;
				}
				pos_ = (pos_ + 1 < text_.size()) ? pos_ + 2 : text_.size();
				continue;
			}
			break;
		}
	}

	// One token; false at the end of the input or at a '}'.
	bool ReadToken(std::string& out) {
		SkipSpace();
		if (pos_ >= text_.size() || text_[pos_] == '}') {
			return false;
		}
		out.clear();
		if (text_[pos_] == '"') {
			++pos_;
			while (pos_ < text_.size()) {
				const char c = text_[pos_++];
				if (c == '\\' && pos_ < text_.size()) {
					const char esc = text_[pos_++];
					switch (esc) {
					case 'n':
						out.push_back('\n');
						break;
					case 'r':
						out.push_back('\r');
						break;
					case 't':
						out.push_back('\t');
						break;
					case '"':
						out.push_back('"');
						break;
					case '\\':
						out.push_back('\\');
						break;
					default:
						out.push_back('\\');
						out.push_back(esc);
						break;
					}
					continue;
				}
				if (c == '"') {
					return true;
				}
				out.push_back(c);
			}
			failed_ = true; // unterminated string
			return true;
		}
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == '"') {
				break;
			}
			if (c == '/' && pos_ + 1 < text_.size() && (text_[pos_ + 1] == '/' || text_[pos_ + 1] == '*')) {
				break;
			}
			out.push_back(c);
			++pos_;
		}
		return true;
	}

	bool PeekIs(char c) {
		SkipSpace();
		return pos_ < text_.size() && text_[pos_] == c;
	}

	// Children are appended as soon as they are complete, so the recursion below works on a local
	// node and never holds a reference into a vector that the recursion may reallocate.
	void ParseNodes(KvNode& parent, bool insideBlock) {
		while (true) {
			SkipSpace();
			if (pos_ >= text_.size()) {
				if (insideBlock) {
					failed_ = true; // missing '}'
				}
				return;
			}
			if (text_[pos_] == '}') {
				if (!insideBlock) {
					failed_ = true; // stray '}'
				}
				++pos_;
				return;
			}
			std::string key;
			if (!ReadToken(key)) {
				return;
			}
			if (!key.empty() && key[0] == '#') {
				// #base / #include / #define: no inclusion, skip the line.
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			KvNode node;
			node.name = std::move(key);
			if (!PeekIs('{')) {
				std::string value;
				if (ReadToken(value)) {
					node.value = std::move(value);
					node.hasValue = true;
				}
			}
			if (PeekIs('{')) {
				++pos_;
				node.isBlock = true;
				ParseNodes(node, true);
			}
			parent.children.push_back(std::move(node));
		}
	}

	std::string_view text_;
	std::size_t pos_ = 0;
	bool failed_ = false;
};

} // namespace detail

inline const KvNode* KvNode::Find(std::string_view key) const {
	if (key.empty()) {
		return nullptr;
	}
	for (const KvNode& child : children) {
		if (detail::EqualNoCase(child.name, key)) {
			return &child;
		}
	}
	return nullptr;
}

inline std::string_view KvNode::GetString(std::string_view key, std::string_view def) const {
	if (key.empty()) {
		return hasValue ? std::string_view(value) : def;
	}
	const KvNode* found = Find(key);
	if (found == nullptr || !found->hasValue) {
		return def;
	}
	return std::string_view(found->value);
}

inline int KvNode::GetInt(std::string_view key, int def) const {
	const std::string_view text = detail::Trim(GetString(key, {}));
	if (text.empty()) {
		return def;
	}
	int value = 0;
	const auto* begin = text.data();
	const auto* end = text.data() + text.size();
	const std::from_chars_result result = std::from_chars(begin, end, value);
	if (result.ec != std::errc{}) {
		return detail::Atoi(text); // Valve falls back to atoi semantics
	}
	return value;
}

inline bool KvNode::GetBool(std::string_view key, bool def) const {
	const std::string_view text = detail::Trim(GetString(key, {}));
	if (text.empty()) {
		return def;
	}
	if (detail::EqualNoCase(text, "true") || detail::EqualNoCase(text, "yes") ||
		detail::EqualNoCase(text, "on")) {
		return true;
	}
	if (detail::EqualNoCase(text, "false") || detail::EqualNoCase(text, "no") ||
		detail::EqualNoCase(text, "off")) {
		return false;
	}
	return detail::Atoi(text) != 0;
}

// The file root: an unnamed node whose children are the top-level entries - except that, like
// Valve's loader (and pisexcompat's shim), a file that is one "BlockDB" { ... } block is returned
// as the *contents* of that block, so `GetString("server_name")` reads a key inside it.
inline std::optional<KvNode> ParseKv1(std::string_view text) {
	// A UTF-8 BOM would otherwise become part of the first key and hide every entry from lookups.
	constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
	if (text.starts_with(kUtf8Bom)) {
		text.remove_prefix(kUtf8Bom.size());
	}
	KvNode root;
	detail::Parser parser(text);
	if (!parser.Parse(root)) {
		return std::nullopt;
	}
	// The single top-level block becomes the root (its name is only used for logging in Valve).
	if (root.children.size() == 1 && root.children.front().isBlock) {
		KvNode top = std::move(root.children.front());
		root.name = std::move(top.name);
		root.value = std::move(top.value);
		root.hasValue = top.hasValue;
		root.children = std::move(top.children);
	}
	return root;
}

inline std::optional<KvNode> LoadKv1File(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	std::ostringstream content;
	content << file.rdbuf();
	return ParseKv1(content.str());
}

// --- 2. where the config is read from ----------------------------------------------------------

// The admin_system directory every Pisex admin module shares; production BlockDB.ini lives there.
inline constexpr std::string_view kAdminSystemDir = "admin_system";
// The file name of the original module.
inline constexpr std::string_view kConfigFile = "BlockDB.ini";
// The raw path the Metamod module passed to KeyValues::LoadFromFile. pisexcompat's ResolvePath
// tried it first, relative to the server working directory (or absolute if it had been absolute);
// it is kept first here so a live server that still has the legacy file keeps using it.
inline constexpr std::string_view kLegacyRawPath = "addons/configs/admin_system/BlockDB.ini";

inline std::string JoinPath(std::string_view base, std::string_view name) {
	if (base.empty()) {
		return std::string(name);
	}
	if (name.empty()) {
		return std::string(base);
	}
	std::string out(base);
	if (out.back() != '/') {
		out.push_back('/');
	}
	if (name.front() == '/') {
		name.remove_prefix(1);
	}
	out.append(name);
	return out;
}

// The candidate list of pisexcompat's ResolvePath("addons/configs/admin_system/BlockDB.ini"),
// in the same order: FirstExisting() made the first existing file authoritative.
//
//   1. addons/configs/admin_system/BlockDB.ini       (raw path, relative to the server cwd)
//   2. <configs>/admin_system/BlockDB.ini
//   3. <configs>/admin_system/translations/BlockDB.ini
//   4. <data>/admin_system/BlockDB.ini
//
// A missing configs/data directory degrades exactly like pisexcompat's empty roots: the path is
// still relative ("admin_system/..."). The caller (src/plugin.cpp) supplies plg::GetConfigsDir()
// and plg::GetDataDir(); this header never touches the Plugify API. There is deliberately no
// plugin-local candidate: the production file location does not change.
inline std::vector<std::string> ConfigCandidates(std::string_view configsDir, std::string_view dataDir) {
	std::vector<std::string> candidates;
	candidates.reserve(4);
	candidates.emplace_back(kLegacyRawPath);
	const std::string configDir = JoinPath(configsDir, kAdminSystemDir);
	candidates.push_back(JoinPath(configDir, kConfigFile));
	candidates.push_back(JoinPath(JoinPath(configDir, "translations"), kConfigFile));
	candidates.push_back(JoinPath(JoinPath(dataDir, kAdminSystemDir), kConfigFile));
	return candidates;
}

inline bool FileExists(const std::string& path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file(path, std::ios::binary);
	return file.good();
}

// --- 3. the config -----------------------------------------------------------------------------

// The keys of the original with the original's defaults: the Metamod module left g_szServerName /
// g_szApiKey / g_szWebHook empty and g_iAutoBlockBanCount 0 until LoadFromFile succeeded.
struct Config {
	std::string serverName;
	std::string apiKey;
	int autoBlockBanCount = 0;
	std::string webhook;
};

// LoadConfig() of the original over an already parsed file: missing keys keep the values already in
// `out`, exactly like the original's GetString/GetInt calls on a KeyValues that has them.
inline void LoadConfigFromRoot(const KvNode& root, Config& out) {
	Config loaded = out;
	loaded.serverName = std::string(root.GetString("server_name", loaded.serverName));
	loaded.apiKey = std::string(root.GetString("ApiKey", loaded.apiKey));
	loaded.autoBlockBanCount = root.GetInt("AutoBlockBanCount", loaded.autoBlockBanCount);
	loaded.webhook = std::string(root.GetString("webhook", loaded.webhook));
	out = loaded;
}

// Parse `text` and replace `out` with its keys. False (and `out` untouched) when the text is
// malformed - the caller then keeps its defaults.
inline bool LoadConfigFromText(std::string_view text, Config& out) {
	const std::optional<KvNode> root = ParseKv1(text);
	if (!root) {
		return false;
	}
	LoadConfigFromRoot(*root, out);
	return true;
}

// Same over a file; false when the file is missing or malformed.
inline bool LoadConfigFromFile(const std::string& path, Config& out) {
	const std::optional<KvNode> root = LoadKv1File(path);
	if (!root) {
		return false;
	}
	LoadConfigFromRoot(*root, out);
	return true;
}

} // namespace as_blockdb
