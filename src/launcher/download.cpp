#include "launcher/download.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <memory>
#include <vector>

#include "launcher/process.h"

namespace launcher {
namespace {

struct Handle {
  HINTERNET h = nullptr;
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
};

}  // namespace

std::string download(const std::wstring& url, const std::filesystem::path& to,
                     const std::function<void(unsigned long long, unsigned long long)>& progress, const std::atomic<bool>& cancel) {
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  wchar_t host[256] = {}, path[2048] = {};
  uc.lpszHostName = host;
  uc.dwHostNameLength = DWORD(std::size(host));
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = DWORD(std::size(path));
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return "not an https address: " + narrow(url);
  Handle session{WinHttpOpen(L"acfa-launcher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.h) return "the internet connection could not be opened";
  Handle conn{WinHttpConnect(session.h, host, uc.nPort, 0)};
  if (!conn.h) return "cannot reach " + narrow(host);
  Handle req{WinHttpOpenRequest(conn.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
  if (!req.h) return "cannot request " + narrow(url);
  if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(req.h, nullptr))
    return "download failed (" + narrow(host) + ", error " + std::to_string(GetLastError()) + "): check the internet connection";
  DWORD status = 0, size = sizeof status;
  WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                      WINHTTP_NO_HEADER_INDEX);
  if (status != 200) return "download failed: the server answered " + std::to_string(status) + " for " + narrow(url);
  wchar_t lenbuf[32] = {};
  DWORD lensize = sizeof lenbuf;
  unsigned long long total = 0;
  if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenbuf, &lensize, WINHTTP_NO_HEADER_INDEX))
    total = _wcstoui64(lenbuf, nullptr, 10);
  std::filesystem::path part = to;
  part += ".part";
  std::error_code ec;
  std::filesystem::create_directories(to.parent_path(), ec);
  FILE* f = _wfopen(part.c_str(), L"wb");
  if (!f) return "cannot write " + narrow(part.wstring());
  std::vector<char> buf(1 << 20);
  unsigned long long got = 0;
  std::string err;
  for (;;) {
    if (cancel.load()) {
      err = "cancelled";
      break;
    }
    DWORD n = 0;
    if (!WinHttpReadData(req.h, buf.data(), DWORD(buf.size()), &n)) {
      err = "download interrupted (error " + std::to_string(GetLastError()) + ")";
      break;
    }
    if (!n) break;
    if (std::fwrite(buf.data(), 1, n, f) != n) {
      err = "cannot write " + narrow(part.wstring()) + " (disk full?)";
      break;
    }
    got += n;
    progress(got, total);
  }
  std::fclose(f);
  if (err.empty() && total && got != total) err = "download incomplete";
  if (!err.empty()) {
    std::filesystem::remove(part, ec);
    return err;
  }
  std::filesystem::rename(part, to, ec);
  if (ec) return "cannot rename " + narrow(part.wstring());
  return "";
}

}  // namespace launcher
