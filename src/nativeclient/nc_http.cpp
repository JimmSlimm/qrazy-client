#include "nc_http.h"
#include "nc_platform.h"
#include <cstring>

#if defined(NC_WINDOWS)
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace nc {

static constexpr char kCoreOrigin[] = "https://qrazy-game.onrender.com";
static constexpr char kStagingOrigin[] = "http://localhost:5173";

#if defined(NC_WINDOWS)
bool httpGet(const std::string& path, bool staging, std::vector<uint8_t>& body, size_t maxBytes, std::string& error, int timeoutMs) {
  body.clear();
  if (path.rfind(staging ? "/native/staging/" : "/native/", 0) != 0) { error = "invalid core path"; return false; }
  const std::string url = std::string(staging ? kStagingOrigin : kCoreOrigin) + path;
  std::wstring w(url.begin(), url.end()); URL_COMPONENTS uc; memset(&uc, 0, sizeof uc); uc.dwStructSize = sizeof uc;
  wchar_t host[256], requestPath[2048]; uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = requestPath; uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) { error = "bad address"; return false; }
  HINTERNET s = WinHttpOpen(L"Qrazy", staging ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0); if (!s) { error = "WinHTTP unavailable"; return false; }
  WinHttpSetTimeouts(s, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
  HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0); HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", requestPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, staging ? 0 : WINHTTP_FLAG_SECURE) : nullptr;
  bool ok = false;
  DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if (r && WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect) && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
    DWORD status = 0, sz = sizeof status; WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
    if (status != 200) error = "HTTP " + std::to_string(status);
    else {
      ok = true; DWORD avail = 0;
      for (;;) {
        if (!WinHttpQueryDataAvailable(r, &avail)) { error = "read failed"; ok = false; break; }
        if (!avail) break;
        if (body.size() + avail > maxBytes) { error = "too large"; ok = false; break; }
        const size_t at = body.size(); body.resize(at + avail); DWORD got = 0;
        if (!WinHttpReadData(r, body.data() + at, avail, &got)) { error = "read failed"; ok = false; break; } body.resize(at + got);
      }
    }
  } else error = "request failed";
  if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); WinHttpCloseHandle(s); if (!ok) body.clear(); return ok;
}
#else
struct Download {
  std::vector<uint8_t>& body;
  size_t limit;
};
static size_t onData(char* bytes, size_t size, size_t count, void* user) {
  auto& download = *static_cast<Download*>(user);
  const size_t length = size * count;
  if (length > download.limit - download.body.size()) return 0;
  try {
    const auto* start = reinterpret_cast<const uint8_t*>(bytes);
    download.body.insert(download.body.end(), start, start + length);
  } catch (...) {
    return 0;
  }
  return length;
}
bool httpGet(const std::string& path, bool staging, std::vector<uint8_t>& body, size_t maxBytes, std::string& error, int timeoutMs) {
  body.clear();
  if (path.rfind(staging ? "/native/staging/" : "/native/", 0) != 0) { error = "invalid core path"; return false; }
  const std::string url = std::string(staging ? kStagingOrigin : kCoreOrigin) + path;
  static const bool initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
  CURL* c = initialized ? curl_easy_init() : nullptr; if (!c) { error = "curl unavailable"; return false; }
  Download download{body, maxBytes};
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(c, CURLOPT_URL, url.c_str()); curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L); curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeoutMs); curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, (long)timeoutMs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onData); curl_easy_setopt(c, CURLOPT_WRITEDATA, &download); curl_easy_setopt(c, CURLOPT_USERAGENT, "Qrazy");
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, staging ? "http" : "https");
  if (staging) curl_easy_setopt(c, CURLOPT_PROXY, "");
  const CURLcode rc = curl_easy_perform(c); long status = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status); curl_easy_cleanup(c);
  if (rc != CURLE_OK) { error = curl_easy_strerror(rc); body.clear(); return false; }
  if (status != 200) { error = "HTTP " + std::to_string(status); body.clear(); return false; }
  if (body.size() > maxBytes) { error = "too large"; body.clear(); return false; }
  return true;
}
#endif

}  // namespace nc
