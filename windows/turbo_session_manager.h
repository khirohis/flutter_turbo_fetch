#ifndef FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_
#define FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_

#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <mutex>

namespace flutter_turbo_fetch {

class TurboSessionManager {
 public:
  static constexpr int64_t kDefaultMaxRequestsPerHost = 32;

  TurboSessionManager();
  ~TurboSessionManager();

  TurboSessionManager(const TurboSessionManager&) = delete;
  TurboSessionManager& operator=(const TurboSessionManager&) = delete;

  // Acquires the shared synchronous WinHTTP session handle configured with
  // HTTP/2 multiplexing and the specified max connections per server.
  // Automatically recreates the session if previously shut down.
  HINTERNET AcquireSession(int64_t max_requests_per_host);

  // Closes the shared WinHTTP session handle.
  void Shutdown();

  bool IsShutdown() const;
  int64_t current_max_per_host() const;

 private:
  mutable std::mutex mutex_;
  HINTERNET session_handle_ = nullptr;
  int64_t current_max_per_host_ = kDefaultMaxRequestsPerHost;
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_

