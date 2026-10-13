// Downloads for the launcher (WinHTTP): the pinned toolchain archives (HTTPS) and, when the player
// asks for it, Sony's PS3 system software (plain HTTP: Sony's update servers offer no valid HTTPS;
// the file is accepted only with its pinned SHA-256).
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace launcher {

// Downloads url to `to` (through a .part file), reporting (received, total) bytes; total is 0
// when the server does not say. http:// addresses only with allow_http (callers verify a pinned
// hash). Returns "" on success, else the reason.
std::string download(const std::wstring& url, const std::filesystem::path& to,
                     const std::function<void(unsigned long long, unsigned long long)>& progress, const std::atomic<bool>& cancel,
                     bool allow_http = false);

// A small text document (at most max_bytes) into *out; same rules. Returns "" on success.
std::string fetch_text(const std::wstring& url, std::string* out, bool allow_http = false, size_t max_bytes = 1 << 16);

}  // namespace launcher
