#ifndef FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_
#define FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_

#include <windows.h>
#include <winhttp.h>

#include <mutex>

namespace flutter_turbo_fetch {

class TurboSessionManager {
 public:
  TurboSessionManager();
  ~TurboSessionManager();

  TurboSessionManager(const TurboSessionManager&) = delete;
  TurboSessionManager& operator=(const TurboSessionManager&) = delete;

  // Acquires the shared synchronous WinHTTP session handle configured with
  // HTTP/2 multiplexing and OS default connection limits.
  // Automatically recreates the session if previously shut down.
  HINTERNET AcquireSession();

  // Closes the shared WinHTTP session handle.
  void Shutdown();

  bool IsShutdown() const;

 private:
  mutable std::mutex mutex_;
  HINTERNET session_handle_ = nullptr;
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_

