#include "turbo_session_manager.h"

#include <utility>

namespace flutter_turbo_fetch {

TurboSessionManager::TurboSessionManager() = default;

TurboSessionManager::~TurboSessionManager() { Shutdown(); }

HINTERNET TurboSessionManager::EnsureSessionLocked() {
  if (session_handle_ == nullptr) {
    session_handle_ = WinHttpOpen(
        L"flutter_turbo_fetch/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_handle_ == nullptr) {
      session_handle_ = WinHttpOpen(
          L"flutter_turbo_fetch/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }

    if (session_handle_ != nullptr) {
      DWORD http2_flag = WINHTTP_PROTOCOL_FLAG_HTTP2;
      WinHttpSetOption(session_handle_, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL,
                       &http2_flag, sizeof(http2_flag));

      DWORD decompress_flags =
          WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
      WinHttpSetOption(session_handle_, WINHTTP_OPTION_DECOMPRESSION,
                       &decompress_flags, sizeof(decompress_flags));
    }
  }
  return session_handle_;
}

HINTERNET TurboSessionManager::AcquireSession() {
  std::lock_guard<std::mutex> lock(mutex_);
  return EnsureSessionLocked();
}

std::shared_ptr<ScopedWinHttpHandle> TurboSessionManager::AcquireConnection(
    const std::wstring& host,
    INTERNET_PORT port) {
  std::lock_guard<std::mutex> lock(mutex_);

  HINTERNET session = EnsureSessionLocked();
  if (session == nullptr) {
    return nullptr;
  }

  ConnectionKey key{host, port};
  auto it = connection_cache_.find(key);
  if (it != connection_cache_.end()) {
    return it->second;
  }

  HINTERNET h_connect = WinHttpConnect(session, host.c_str(), port, 0);
  if (h_connect == nullptr) {
    return nullptr;
  }

  if (connection_cache_.size() >= kMaxCachedConnections &&
      !connection_order_.empty()) {
    connection_cache_.erase(connection_order_.front());
    connection_order_.pop_front();
  }

  auto handle = std::make_shared<ScopedWinHttpHandle>(h_connect);
  connection_order_.push_back(key);
  connection_cache_.emplace(std::move(key), handle);
  return handle;
}

void TurboSessionManager::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  connection_cache_.clear();
  connection_order_.clear();
  if (session_handle_ != nullptr) {
    WinHttpCloseHandle(session_handle_);
    session_handle_ = nullptr;
  }
}

bool TurboSessionManager::IsShutdown() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_handle_ == nullptr;
}

size_t TurboSessionManager::CachedConnectionCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return connection_cache_.size();
}

}  // namespace flutter_turbo_fetch
