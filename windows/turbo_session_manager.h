#ifndef FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_
#define FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_

#include <windows.h>
#include <winhttp.h>

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace flutter_turbo_fetch {

struct ScopedWinHttpHandle {
  explicit ScopedWinHttpHandle(HINTERNET h = nullptr) : handle(h) {}
  ~ScopedWinHttpHandle() {
    if (handle != nullptr) {
      WinHttpCloseHandle(handle);
    }
  }
  ScopedWinHttpHandle(const ScopedWinHttpHandle&) = delete;
  ScopedWinHttpHandle& operator=(const ScopedWinHttpHandle&) = delete;

  HINTERNET handle = nullptr;
};

class TurboSessionManager {
 public:
  static constexpr size_t kMaxCachedConnections = 64;

  TurboSessionManager();
  ~TurboSessionManager();

  TurboSessionManager(const TurboSessionManager&) = delete;
  TurboSessionManager& operator=(const TurboSessionManager&) = delete;

  // Acquires the shared synchronous WinHTTP session handle configured with
  // HTTP/2 multiplexing and OS default connection limits.
  // Automatically recreates the session if previously shut down.
  HINTERNET AcquireSession();

  // Acquires a cached WinHttpConnect handle for the given (host, port) origin,
  // creating and caching a new one if absent. Returns nullptr on failure
  // (preserving GetLastError()).
  std::shared_ptr<ScopedWinHttpHandle> AcquireConnection(
      const std::wstring& host,
      INTERNET_PORT port);

  // Closes cached connection handles and the shared WinHTTP session handle.
  void Shutdown();

  bool IsShutdown() const;
  size_t CachedConnectionCount() const;

 private:
  struct ConnectionKey {
    std::wstring host;
    INTERNET_PORT port = 0;

    bool operator==(const ConnectionKey& other) const {
      return port == other.port && host == other.host;
    }
  };

  struct ConnectionKeyHash {
    size_t operator()(const ConnectionKey& key) const {
      size_t h1 = std::hash<std::wstring>{}(key.host);
      size_t h2 = std::hash<INTERNET_PORT>{}(key.port);
      return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
  };

  HINTERNET EnsureSessionLocked();

  mutable std::mutex mutex_;
  HINTERNET session_handle_ = nullptr;
  std::unordered_map<ConnectionKey,
                     std::shared_ptr<ScopedWinHttpHandle>,
                     ConnectionKeyHash>
      connection_cache_;
  std::deque<ConnectionKey> connection_order_;
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_
