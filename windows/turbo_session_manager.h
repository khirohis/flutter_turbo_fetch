#ifndef FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_
#define FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_

#include <windows.h>
#include <winhttp.h>

#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

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
      std::wstring_view host,
      INTERNET_PORT port);

  // Closes cached connection handles and the shared WinHTTP session handle.
  void Shutdown();

  bool IsShutdown() const;
  size_t CachedConnectionCount() const;

 private:
  struct ConnectionKey {
    std::wstring host;
    INTERNET_PORT port = 0;
  };

  struct ConnectionKeyView {
    std::wstring_view host;
    INTERNET_PORT port = 0;
  };

  struct ConnectionKeyLess {
    using is_transparent = void;

    static bool Compare(std::wstring_view lhs_host,
                        INTERNET_PORT lhs_port,
                        std::wstring_view rhs_host,
                        INTERNET_PORT rhs_port) {
      if (lhs_port != rhs_port) {
        return lhs_port < rhs_port;
      }
      return lhs_host < rhs_host;
    }

    bool operator()(const ConnectionKey& a, const ConnectionKey& b) const {
      return Compare(a.host, a.port, b.host, b.port);
    }
    bool operator()(const ConnectionKey& a, const ConnectionKeyView& b) const {
      return Compare(a.host, a.port, b.host, b.port);
    }
    bool operator()(const ConnectionKeyView& a, const ConnectionKey& b) const {
      return Compare(a.host, a.port, b.host, b.port);
    }
  };

  HINTERNET EnsureSessionLocked();

  mutable std::mutex mutex_;
  HINTERNET session_handle_ = nullptr;
  std::map<ConnectionKey,
           std::shared_ptr<ScopedWinHttpHandle>,
           ConnectionKeyLess>
      connection_cache_;
  std::deque<ConnectionKey> connection_order_;
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_TURBO_SESSION_MANAGER_H_
