#include "nc_net.h"
#include "nc_vk.h"      // logf
#if defined(NC_WINDOWS)
#include <winhttp.h>
#else
#include <curl/curl.h>
#include <poll.h>
#include <chrono>
#endif
#include <cstring>


namespace nc {

#if defined(NC_WINDOWS)
static std::wstring widen(const std::string& s) { if (s.empty()) return {}; const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0); std::wstring w((size_t)n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w; }
#endif

Net::~Net() { close(); }

#if defined(NC_WINDOWS)
bool Net::connect(const std::string& url) {
  close(); error_.clear(); close_ = {}; stop_ = false;
  if (url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0) {
    error_ = "invalid WebSocket URL";
    return false;
  }
  URL_COMPONENTSW uc{};
  uc.dwStructSize = sizeof uc;
  uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = DWORD(-1);
  const std::wstring wurl = widen(url); std::wstring u = wurl; if (u.compare(0, 6, L"wss://") == 0) u = L"https://" + u.substr(6); else if (u.compare(0, 5, L"ws://") == 0) u = L"http://" + u.substr(5);
  if (!WinHttpCrackUrl(u.c_str(), 0, 0, &uc)) { error_ = "bad url"; return false; }
  const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring path = uc.dwUrlPathLength ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) : L"/";
  if (uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  HINTERNET ses = WinHttpOpen(L"Qrazy-native", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0); if (!ses) { error_ = "WinHttpOpen failed"; return false; }
  WinHttpSetTimeouts(ses, 5000, 5000, 5000, 5000);
  HINTERNET con = WinHttpConnect(ses, host.c_str(), uc.nPort, 0); if (!con) { error_ = "connect failed"; WinHttpCloseHandle(ses); return false; }
  HINTERNET req = WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, nullptr, nullptr, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!req) { error_ = "request failed"; WinHttpCloseHandle(con); WinHttpCloseHandle(ses); return false; }
  const std::wstring origin = std::wstring(L"Origin: ") + (uc.nScheme == INTERNET_SCHEME_HTTPS ? L"https://" : L"http://") + host;   // the hub may check that the socket comes from the game's own origin
  WinHttpAddRequestHeaders(req, origin.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
  if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) || !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(req, nullptr)) {
    error_ = "upgrade failed (" + std::to_string(GetLastError()) + ")"; WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses); return false; }
  HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req, 0); WinHttpCloseHandle(req); if (!ws) { error_ = "websocket upgrade refused"; WinHttpCloseHandle(con); WinHttpCloseHandle(ses); return false; }
  DWORD receiveTimeout = 0;
  WinHttpSetOption(ws, WINHTTP_OPTION_RECEIVE_TIMEOUT, &receiveTimeout, sizeof receiveTimeout);
  session_ = ses; conn_ = con; ws_ = ws; up_ = true;
  th_ = std::thread([this] { run(); });
  writer_ = std::thread([this] { write(); });
  logf("net: WebSocket connected");
  return true;
}

void Net::close() {
  {
    std::lock_guard<std::mutex> lock(sendMu_);
    stop_ = true;
    up_ = false;
  }
  writable_.notify_one();
  if (writer_.joinable()) writer_.join();
  pendingReady_ = false;
  if (ws_) {
    WinHttpWebSocketShutdown((HINTERNET)ws_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    WinHttpCloseHandle((HINTERNET)ws_);
  }
  if (th_.joinable()) th_.join();
  ws_ = nullptr;
  rawCount_ = 0;
  if (conn_) { WinHttpCloseHandle((HINTERNET)conn_); conn_ = nullptr; } if (session_) { WinHttpCloseHandle((HINTERNET)session_); session_ = nullptr; }
}

void Net::write() {
  std::unique_lock<std::mutex> lock(sendMu_);
  while (true) {
    writable_.wait(lock, [&] { return stop_ || pendingReady_; });
    if (stop_) return;
    const auto type = pendingBinary_ ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE :
                                       WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
    uint8_t* data = pendingSize_ > pending_.size() ? largePending_.data() : pending_.data();
    pendingStarted_ = true;
    lock.unlock();
    const DWORD result = WinHttpWebSocketSend((HINTERNET)ws_, type, data, (DWORD)pendingSize_);
    lock.lock();
    if (result != NO_ERROR) {
      std::lock_guard<std::mutex> receiveLock(mu_);
      close_.reason = "send failed (" + std::to_string(result) + ")";
      up_ = false;
      return;
    }
    pendingReady_ = false;
  }
}
#endif

bool Net::sendable() {
  std::lock_guard<std::mutex> lock(sendMu_);
  return up_ && !pendingReady_;
}

void Net::discardUnsentMapState() {
  std::lock_guard<std::mutex> lock(sendMu_);
  if (pendingReady_ && pendingMapState_ && !pendingStarted_) pendingReady_ = false;
}

bool Net::send(bool binary, const uint8_t* data, size_t size, bool mapState) {
  std::lock_guard<std::mutex> lock(sendMu_);
  if (!up_ || pendingReady_) return false;
  if (size > pending_.size()) largePending_.assign(data, data + size);
  else if (size) std::memcpy(pending_.data(), data, size);
  pendingSize_ = size;
  pendingBinary_ = binary;
  pendingMapState_ = mapState;
  pendingStarted_ = false;
  pendingReady_ = true;
#if defined(NC_WINDOWS)
  writable_.notify_one();
#endif
  return true;
}

void Net::receive(bool binary, std::string& message) {
  std::lock_guard<std::mutex> lock(mu_);
  if (rawCount_ < 4096) {
    if (rawCount_ == raw_.size()) raw_.emplace_back();
    auto& slot = raw_[rawCount_++];
    slot.binary = binary;
    slot.data.assign(message);
  } else {
    close_.reason = "receive queue full";
    up_ = false;
  }
  message.clear();
}

size_t Net::poll(std::vector<NetRaw>& out) {
  std::lock_guard<std::mutex> lock(mu_);
  out.swap(raw_);
  const size_t count = rawCount_;
  rawCount_ = 0;
  return count;
}

NetClose Net::closeInfo() {
  std::lock_guard<std::mutex> lock(mu_);
  return close_;
}

#if defined(NC_WINDOWS)
void Net::run() {
  std::string message;
  std::array<char, 65536> buffer;
  const HINTERNET socket = (HINTERNET)ws_;
  while (!stop_ && up_) {
    DWORD got = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
    const DWORD err = WinHttpWebSocketReceive(socket, buffer.data(), (DWORD)buffer.size(), &got, &type);
    if (err != NO_ERROR) {
      std::lock_guard<std::mutex> lock(mu_);
      close_.reason = "connection lost (" + std::to_string(err) + ")";
      break;
    }
    if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
      USHORT code = 0;
      DWORD length = 0;
      if (WinHttpWebSocketQueryCloseStatus(socket, &code, buffer.data(), 123, &length) == NO_ERROR) {
        std::lock_guard<std::mutex> lock(mu_);
        close_ = {code, std::string(buffer.data(), length), true};
      }
      break;
    }
    message.append(buffer.data(), got);
    if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
        type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
      receive(type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, message);
  }
  up_ = false;
}

#else
// ---- Linux transport: libcurl's WebSocket API (CURLOPT_CONNECT_ONLY 2). One worker thread owns the easy handle: it sends what the other threads queued and receives in between.
bool Net::connect(const std::string& url) {
  close(); error_.clear(); close_ = {}; stop_ = false;
  if (url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0) {
    error_ = "invalid WebSocket URL";
    return false;
  }
  static const bool globalInit = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK); (void)globalInit;
  CURL* e = curl_easy_init(); if (!e) { error_ = "curl_easy_init failed"; return false; }
  std::string origin = url; if (origin.compare(0, 6, "wss://") == 0) origin = "https://" + origin.substr(6); else if (origin.compare(0, 5, "ws://") == 0) origin = "http://" + origin.substr(5);
  const size_t slash = origin.find('/', origin.find("://") + 3); if (slash != std::string::npos) origin.resize(slash);   // the hub may check that the socket comes from the game's own origin
  struct curl_slist* hdr = curl_slist_append(nullptr, ("Origin: " + origin).c_str());
  curl_easy_setopt(e, CURLOPT_URL, url.c_str()); curl_easy_setopt(e, CURLOPT_CONNECT_ONLY, 2L); curl_easy_setopt(e, CURLOPT_HTTPHEADER, hdr); curl_easy_setopt(e, CURLOPT_USERAGENT, "Qrazy-native");
  curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  const CURLcode rc = curl_easy_perform(e); curl_slist_free_all(hdr);
  if (rc != CURLE_OK) { error_ = std::string("websocket upgrade failed: ") + curl_easy_strerror(rc); curl_easy_cleanup(e); return false; }
  ws_ = e; up_ = true;
  th_ = std::thread([this] { run(); });
  logf("net: WebSocket connected");
  return true;
}

void Net::close() {
  stop_ = true; up_ = false; if (th_.joinable()) th_.join();
  if (ws_) { size_t sent = 0; curl_ws_send((CURL*)ws_, "", 0, &sent, 0, CURLWS_CLOSE); curl_easy_cleanup((CURL*)ws_); ws_ = nullptr; }
  pendingReady_ = false;
  rawCount_ = 0;
}

void Net::run() {
  CURL* socket = (CURL*)ws_;
  std::array<char, 65536> buffer;
  std::string message, closeReason;
  bool binary = false, assembling = false;
  size_t offset = 0;
  while (!stop_ && up_) {
    bool waitingToSend = false;
    {
      std::lock_guard<std::mutex> lock(sendMu_);
      if (pendingReady_) {
        size_t sent = 0;
        const uint8_t* data = pendingSize_ > pending_.size() ? largePending_.data() : pending_.data();
        pendingStarted_ = true;
        const auto rc = curl_ws_send(socket, data + offset, pendingSize_ - offset, &sent, 0,
                                     pendingBinary_ ? CURLWS_BINARY : CURLWS_TEXT);
        offset += sent;
        if (rc != CURLE_OK && rc != CURLE_AGAIN) {
          closeReason = curl_easy_strerror(rc);
          break;
        }
        if (rc == CURLE_OK && offset == pendingSize_) {
          pendingReady_ = false;
          offset = 0;
        }
        waitingToSend = pendingReady_;
      }
    }
    size_t got = 0;
    const struct curl_ws_frame* meta = nullptr;
    const auto rc = curl_ws_recv(socket, buffer.data(), buffer.size(), &got, &meta);
    if (rc == CURLE_AGAIN) {
      curl_socket_t fd = CURL_SOCKET_BAD;
      curl_easy_getinfo(socket, CURLINFO_ACTIVESOCKET, &fd);
      if (fd != CURL_SOCKET_BAD) { struct pollfd p = {fd, short(POLLIN | (waitingToSend ? POLLOUT : 0)), 0}; ::poll(&p, 1, 2); }
      else std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    if (rc != CURLE_OK) { closeReason = curl_easy_strerror(rc); break; }
    if (!meta) continue;
    if (meta->flags & CURLWS_CLOSE) {
      std::lock_guard<std::mutex> lock(mu_);
      close_ = got >= 2 ? NetClose{(uint8_t(buffer[0]) << 8) | uint8_t(buffer[1]),
                                   std::string(buffer.data() + 2, got - 2), true}
                        : NetClose{1005, {}, true};
      break;
    }
    if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;
    if (!assembling) {
      if (!(meta->flags & (CURLWS_TEXT | CURLWS_BINARY))) continue;
      binary = (meta->flags & CURLWS_BINARY) != 0;
      assembling = true;
    }
    message.append(buffer.data(), got);
    if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
      receive(binary, message);
      assembling = false;
    }
  }
  if (!closeReason.empty()) {
    std::lock_guard<std::mutex> lock(mu_);
    close_.reason = std::move(closeReason);
  }
  up_ = false;
}

#endif


}  // namespace nc
