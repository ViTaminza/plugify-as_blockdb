// as_blockdb — safe URL formatting for logs.
//
// The Steam HTTP helper logs the URL of a request when the transport refuses it. Some of those
// URLs are secrets: the Discord webhook URL contains its token in the path
// (https://discord.com/api/webhooks/<id>/<token>). A log line must therefore never contain the
// path, the query or the userinfo - only the origin (scheme://host[:port]) or a fixed placeholder.
//
// SafeUrlForLog() is a pure function (no engine, no Plugify), so it is unit-tested directly; the
// transport error branches that use it are tested with an injected fake Steam API.

#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace steamhttp {

namespace detail {

inline bool IsSchemeChar(char c) {
	const auto byte = static_cast<unsigned char>(c);
	return std::isalnum(byte) != 0 || c == '+' || c == '-' || c == '.';
}

inline bool IsValidScheme(std::string_view scheme) {
	if (scheme.empty() || scheme.size() > 16) {
		return false;
	}
	if (std::isalpha(static_cast<unsigned char>(scheme.front())) == 0) {
		return false;
	}
	for (const char c : scheme) {
		if (!IsSchemeChar(c)) {
			return false;
		}
	}
	return true;
}

inline bool IsDigits(std::string_view text) {
	if (text.empty()) {
		return false;
	}
	for (const char c : text) {
		if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
			return false;
		}
	}
	return true;
}

inline bool IsValidAuthority(std::string_view authority) {
	if (authority.empty()) {
		return false;
	}
	// IPv6 literal: [::1] with an optional :port suffix.
	if (authority.front() == '[') {
		const auto close = authority.find(']');
		if (close == std::string_view::npos || close == 1) {
			return false;
		}
		for (std::size_t i = 1; i < close; ++i) {
			const char c = authority[i];
			if (std::isxdigit(static_cast<unsigned char>(c)) == 0 && c != ':' && c != '.') {
				return false;
			}
		}
		const std::string_view rest = authority.substr(close + 1);
		if (rest.empty()) {
			return true;
		}
		return rest.front() == ':' && IsDigits(rest.substr(1)) &&
			   rest.find(':') == 0; // only one ':' right after ']'
	}
	const auto colon = authority.rfind(':');
	std::string_view host = authority;
	if (colon != std::string_view::npos) {
		if (authority.find(':') != colon) {
			return false; // an unbracketed IPv6 is not a valid authority we want to print
		}
		if (!IsDigits(authority.substr(colon + 1))) {
			return false;
		}
		host = authority.substr(0, colon);
	}
	if (host.empty()) {
		return false;
	}
	for (const char c : host) {
		const auto byte = static_cast<unsigned char>(c);
		if (std::isalnum(byte) == 0 && c != '.' && c != '-' && c != '_') {
			return false;
		}
	}
	return true;
}

} // namespace detail

// "https://discord.com/api/webhooks/123/SECRET?x=1" -> "https://discord.com"
// "https://user:pass@host:8443/x"                  -> "https://host:8443"
// "https://[::1]:8080/x"                           -> "https://[::1]:8080"
// anything malformed                               -> "<redacted-url>"
inline std::string SafeUrlForLog(std::string_view url) {
	constexpr std::string_view kRedacted = "<redacted-url>";
	// A valid URL is percent-encoded; an embedded space/control byte means the string is not
	// trustworthy, and truncating it at the space would print a misleading host.
	for (const char c : url) {
		const auto byte = static_cast<unsigned char>(c);
		if (std::isspace(byte) != 0 || std::iscntrl(byte) != 0) {
			return std::string(kRedacted);
		}
	}
	const auto schemeEnd = url.find("://");
	if (schemeEnd == std::string_view::npos) {
		return std::string(kRedacted);
	}
	const std::string_view scheme = url.substr(0, schemeEnd);
	if (!detail::IsValidScheme(scheme)) {
		return std::string(kRedacted);
	}

	std::string_view rest = url.substr(schemeEnd + 3);
	const auto authorityEnd = rest.find_first_of("/?# \t\r\n\f\v");
	std::string_view authority = rest.substr(0, authorityEnd);

	// Strip userinfo; only the last '@' before the path is the separator.
	if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
		authority.remove_prefix(at + 1);
	}
	if (!detail::IsValidAuthority(authority)) {
		return std::string(kRedacted);
	}

	std::string out;
	out.reserve(scheme.size() + 3 + authority.size());
	out.append(scheme);
	out.append("://");
	out.append(authority);
	return out;
}

} // namespace steamhttp
