#include "turbo_session_manager.h"

#include <algorithm>

namespace flutter_turbo_fetch {

namespace {

void ConfigureMaxConnsPerServer(HINTERNET session, int64_t max_per_host) {
  DWORD max_conns = static_cast<DWORD>(std::max<int64_t>(1, max_per_host));
  WinHttpSetOption(session, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &max_conns,
                   sizeof(max_conns));
  WinHttpSetOption(session, WINHTTP_OPTION_MAX_CONNS_PER_1_0_SERVER, &max_conns,
                   sizeof(max_conns));
}

}  // namespace

TurboSessionManager::TurboSessionManager() = default;

TurboSessionManager::~TurboSessionManager() { Shutdown(); }

HINTERNET TurboSessionManager::AcquireSession(int64_t max_requests_per_host) {
  std::lock_guard<std::mutex> lock(mutex_);
  const int64_t safe_per_host = std::max<int64_t>(1, max_requests_per_host);

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

      ConfigureMaxConnsPerServer(session_handle_, safe_per_host);
      current_max_per_host_ = safe_per_host;
    }
  } else if (current_max_per_host_ != safe_per_host) {
    ConfigureMaxConnsPerServer(session_handle_, safe_per_host);
    current_max_per_host_ = safe_per_host;
  }

  return session_handle_;
}

void TurboSessionManager::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (session_handle_ != nullptr) {
    WinHttpCloseHandle(session_handle_);
    session_handle_ = nullptr;
  }
}

bool TurboSessionManager::IsShutdown() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_handle_ == nullptr;
}

int64_t TurboSessionManager::current_max_per_host() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_max_per_host_;
}

}  // namespace flutter_turbo_fetch

