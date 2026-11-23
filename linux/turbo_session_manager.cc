#include "turbo_session_manager.h"

#include <atomic>

namespace flutter_turbo_fetch {

namespace {

// Global curl initialization tracker.
std::atomic<int> g_global_init_ref_count{0};

void EnsureCurlGlobalInit() {
  if (g_global_init_ref_count.fetch_add(1) == 0) {
    curl_global_init(CURL_GLOBAL_ALL);
  }
}

void EnsureCurlGlobalCleanup() {
  if (g_global_init_ref_count.fetch_sub(1) == 1) {
    curl_global_cleanup();
  }
}

}  // namespace

TurboSessionManager::TurboSessionManager() {
  EnsureCurlGlobalInit();
}

TurboSessionManager::~TurboSessionManager() {
  Shutdown();
  EnsureCurlGlobalCleanup();
}

void TurboSessionManager::InitializeMultiHandleLocked() {
  if (multi_handle_ != nullptr) {
    return;
  }

  multi_handle_ = curl_multi_init();
  if (multi_handle_ == nullptr) {
    return;
  }

  // Enable HTTP/2 stream multiplexing across the single event-loop connection pool.
  curl_multi_setopt(multi_handle_, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
  is_shutdown_ = false;
}

CURLM* TurboSessionManager::AcquireMultiHandle() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (multi_handle_ == nullptr || is_shutdown_) {
    InitializeMultiHandleLocked();
  }

  return multi_handle_;
}

void TurboSessionManager::Wakeup() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (multi_handle_ != nullptr && !is_shutdown_) {
    curl_multi_wakeup(multi_handle_);
  }
}

void TurboSessionManager::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (multi_handle_ != nullptr) {
    curl_multi_cleanup(multi_handle_);
    multi_handle_ = nullptr;
  }
  is_shutdown_ = true;
}

bool TurboSessionManager::IsShutdown() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return is_shutdown_;
}

}  // namespace flutter_turbo_fetch
