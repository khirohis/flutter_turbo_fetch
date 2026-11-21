#include "turbo_session_manager.h"

namespace flutter_turbo_fetch {

TurboSessionManager::TurboSessionManager() = default;

TurboSessionManager::~TurboSessionManager() { Shutdown(); }

HINTERNET TurboSessionManager::AcquireSession() {
  std::lock_guard<std::mutex> lock(mutex_);

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

}  // namespace flutter_turbo_fetch

