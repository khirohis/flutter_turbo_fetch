#include "turbo_session_manager.h"

#include <algorithm>
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

void TurboSessionManager::LockCallback(CURL* /*handle*/, curl_lock_data data,
                                       curl_lock_access /*access*/, void* userptr) {
  auto* self = static_cast<TurboSessionManager*>(userptr);
  if (data < CURL_LOCK_DATA_LAST) {
    self->share_mutexes_[data].lock();
  }
}

void TurboSessionManager::UnlockCallback(CURL* /*handle*/, curl_lock_data data,
                                         void* userptr) {
  auto* self = static_cast<TurboSessionManager*>(userptr);
  if (data < CURL_LOCK_DATA_LAST) {
    self->share_mutexes_[data].unlock();
  }
}

void TurboSessionManager::InitializeShareHandleLocked() {
  if (share_handle_ != nullptr) {
    return;
  }

  share_handle_ = curl_share_init();
  if (share_handle_ == nullptr) {
    return;
  }

  curl_share_setopt(share_handle_, CURLSHOPT_USERDATA, this);
  curl_share_setopt(share_handle_, CURLSHOPT_LOCKFUNC, &TurboSessionManager::LockCallback);
  curl_share_setopt(share_handle_, CURLSHOPT_UNLOCKFUNC, &TurboSessionManager::UnlockCallback);

  // Enable HTTP/2 connection reuse, DNS cache, and SSL session reuse
  curl_share_setopt(share_handle_, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
  curl_share_setopt(share_handle_, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
  curl_share_setopt(share_handle_, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);

  is_shutdown_ = false;
}

CURLSH* TurboSessionManager::AcquireShareHandle(int64_t max_requests_per_host) {
  std::lock_guard<std::mutex> lock(mutex_);

  const int64_t safe_limit = std::max<int64_t>(1, max_requests_per_host);
  current_max_per_host_ = safe_limit;

  if (share_handle_ == nullptr || is_shutdown_) {
    InitializeShareHandleLocked();
  }

  return share_handle_;
}

void TurboSessionManager::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (share_handle_ != nullptr) {
    curl_share_cleanup(share_handle_);
    share_handle_ = nullptr;
  }
  is_shutdown_ = true;
}

bool TurboSessionManager::IsShutdown() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return is_shutdown_;
}

int64_t TurboSessionManager::current_max_per_host() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_max_per_host_;
}

}  // namespace flutter_turbo_fetch
