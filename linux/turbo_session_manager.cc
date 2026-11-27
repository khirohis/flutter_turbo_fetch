#include "turbo_session_manager.h"

#include <mutex>

namespace flutter_turbo_fetch {

namespace {

// Mutex-guarded global curl initialization tracker so concurrent
// TurboSessionManager lifecycles cannot race curl_global_init/cleanup.
std::mutex& GlobalCurlInitMutex() {
  static std::mutex mtx;
  return mtx;
}

int g_global_init_ref_count = 0;

bool EnsureCurlGlobalInit() {
  std::lock_guard<std::mutex> lock(GlobalCurlInitMutex());
  if (g_global_init_ref_count == 0) {
    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
      return false;
    }
  }
  ++g_global_init_ref_count;
  return true;
}

void EnsureCurlGlobalCleanup() {
  std::lock_guard<std::mutex> lock(GlobalCurlInitMutex());
  if (g_global_init_ref_count <= 0) {
    return;
  }
  --g_global_init_ref_count;
  if (g_global_init_ref_count == 0) {
    curl_global_cleanup();
  }
}

}  // namespace

TurboSessionManager::TurboSessionManager()
    : global_init_acquired_(EnsureCurlGlobalInit()) {}

TurboSessionManager::~TurboSessionManager() {
  Shutdown();
  if (global_init_acquired_) {
    EnsureCurlGlobalCleanup();
  }
}

void TurboSessionManager::InitializeMultiHandleLocked() {
  if (!global_init_acquired_ || multi_handle_ != nullptr) {
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
