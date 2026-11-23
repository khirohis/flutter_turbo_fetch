#ifndef FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_
#define FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_

#include <curl/curl.h>

#include <mutex>

namespace flutter_turbo_fetch {

class TurboSessionManager {
 public:
  TurboSessionManager();
  ~TurboSessionManager();

  TurboSessionManager(const TurboSessionManager&) = delete;
  TurboSessionManager& operator=(const TurboSessionManager&) = delete;

  // Acquires the shared CURLM handle configured with HTTP/2 multiplexing
  // (CURLPIPE_MULTIPLEX). Automatically recreates the handle if previously shut down.
  // Note: CURLM operations (except curl_multi_wakeup via Wakeup()) must be
  // performed exclusively on the single event-loop thread.
  CURLM* AcquireMultiHandle();

  // Thread-safely wakes up curl_multi_poll() on the event-loop thread if active.
  void Wakeup();

  // Closes the CURLM handle and releases cached connection pools.
  // Must be called only after TurboHttpExecutor::Shutdown() has removed all easy
  // handles and joined the event-loop thread.
  void Shutdown();

  bool IsShutdown() const;

 private:
  void InitializeMultiHandleLocked();

  mutable std::mutex mutex_;
  CURLM* multi_handle_ = nullptr;
  bool is_shutdown_ = false;
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_

