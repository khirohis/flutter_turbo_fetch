#ifndef FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_
#define FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_

#include <curl/curl.h>

#include <mutex>
#include <array>

namespace flutter_turbo_fetch {

class TurboSessionManager {
 public:
  TurboSessionManager();
  ~TurboSessionManager();

  TurboSessionManager(const TurboSessionManager&) = delete;
  TurboSessionManager& operator=(const TurboSessionManager&) = delete;

  // Acquires the shared CURLSH handle configured with connection, DNS, and TLS
  // session caching. Automatically recreates the handle if previously shut down.
  CURLSH* AcquireShareHandle();

  // Closes the shared CURLSH handle and releases cached connection pools.
  void Shutdown();

  bool IsShutdown() const;

 private:
  void InitializeShareHandleLocked();

  static void LockCallback(CURL* handle, curl_lock_data data,
                           curl_lock_access access, void* userptr);
  static void UnlockCallback(CURL* handle, curl_lock_data data, void* userptr);

  mutable std::mutex mutex_;
  CURLSH* share_handle_ = nullptr;
  bool is_shutdown_ = false;

  // Mutexes for CURLSH data categories (connect, dns, ssl_session, etc.)
  std::array<std::mutex, CURL_LOCK_DATA_LAST> share_mutexes_;
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_

